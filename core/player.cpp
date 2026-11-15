#include "player.hpp"

#include "platform.hpp"

namespace nxp {

bool Player::start(std::string& err) {
    demux_ = new MkvDemuxer(src_);
    if (!demux_->open()) {
        err = "stream: " + demux_->error();
        return false;
    }
    const MkvTrack* v = demux_->find_track(TrackKind::Video);
    const MkvTrack* a = demux_->find_track(TrackKind::Audio);
    if (v && v->codec_id == "V_MJPEG") vtrack_ = v->number;
    if (a && a->codec_id == "A_MPEG/L3") atrack_ = a->number;
    if (!vtrack_ && !atrack_) {
        err = "stream has no playable tracks (need V_MJPEG and/or A_MPEG/L3; got";
        for (auto& t : demux_->tracks()) err += " " + t.codec_id;
        err += ")";
        return false;
    }
    if (atrack_ && !audio_.open(48000)) {
        logf("player: audio output unavailable, playing video only");
        atrack_ = 0;
    }
    logf("player: video track %llu, audio track %llu, duration %lld ms", (unsigned long long)vtrack_,
         (unsigned long long)atrack_, (long long)demux_->duration_ms());
    if (!start_threads()) {
        err = "could not start player threads";
        stop();
        return false;
    }
    return true;
}

bool Player::start_threads() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = false;
        eof_ = false;
        audio_done_ = !atrack_;
    }
    if (!reader_thread_.start(reader_entry, this, 0x8000, 0x3B)) return false;
    if (atrack_ && !audio_thread_.start(audio_entry, this, 0x10000, 0x30)) return false;
    return true;
}

void Player::stop_threads() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    reader_thread_.join();
    audio_thread_.join();
}

bool Player::seek(int64_t target) {
    if (!demux_ || !can_seek()) return false;
    stop_threads();
    int64_t dur = demux_->duration_ms();
    int64_t size = src_.size();
    if (dur > 0 && target > dur - 2000) target = dur - 2000;
    if (target < 0) target = 0;
    double bytes_per_ms = dur > 0 ? (double)size / (double)dur : 60.0;

    // Estimate the byte offset from the average bitrate, then correct using
    // the timestamp of the first packet found there.
    std::deque<MkvPacket> carry;
    bool found = false;
    double pos = (double)target * bytes_per_ms;
    for (int attempt = 0; attempt < 5 && !found; attempt++) {
        if (pos < 0) pos = 0;
        carry.clear();
        if (!demux_->seek_bytes((uint64_t)pos)) {
            pos -= 2.0 * 1024 * 1024;  // probably past the end: back off
            continue;
        }
        MkvPacket pkt;
        if (demux_->next(pkt) != 1) {
            pos -= 2.0 * 1024 * 1024;
            continue;
        }
        int64_t got = pkt.pts_ms;
        carry.push_back(std::move(pkt));
        if (got > target + 300 && pos > 0) {
            pos -= (double)(got - target + 2000) * bytes_per_ms;  // overshot: go back a bit further
        } else if (got < target - 20000) {
            pos += (double)(target - got - 2000) * bytes_per_ms;  // far too early
        } else {
            found = true;
        }
    }
    if (!found && carry.empty()) {
        logf("player: seek to %lld ms failed: %s", (long long)target, demux_->error().c_str());
        start_threads();  // resume wherever the demuxer is
        return false;
    }
    audio_.flush();
    {
        std::lock_guard<std::mutex> lk(mu_);
        vq_.clear();
        aq_.clear();
        queue_bytes_ = 0;
        carry_ = std::move(carry);
        skip_until_ms_ = target;
        last_pos_ms_ = target;
        base_pts_ = -1;
        wall_start_ms_ = 0;
        paused_total_ms_ = 0;
        paused_at_ms_ = paused_ ? now_ms() : 0;
        first_frame_shown_ = false;
        error_.clear();
    }
    logf("player: seek to %lld ms", (long long)target);
    return start_threads();
}

void Player::stop() {
    stop_threads();
    if (atrack_) audio_.close();
    atrack_ = 0;
    delete demux_;
    demux_ = nullptr;
}

void Player::reader_loop() {
    for (;;) {
        MkvPacket pkt;
        int r = 1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!carry_.empty()) {
                pkt = std::move(carry_.front());
                carry_.pop_front();
                r = 2;
            }
        }
        if (r != 2) r = demux_->next(pkt);
        std::unique_lock<std::mutex> lk(mu_);
        if (r <= 0) {
            if (r < 0 && !stopping_) error_ = "stream: " + demux_->error();
            eof_ = true;
            cv_.notify_all();
            return;
        }
        if (pkt.track != vtrack_ && pkt.track != atrack_) continue;
        if (skip_until_ms_ >= 0) {
            if (pkt.pts_ms < skip_until_ms_) continue;  // before the seek target
            skip_until_ms_ = -1;
        }
        // Back-pressure. Never block while the *other* queue is empty, or the
        // consumer of that queue could starve and the clock would stall.
        cv_.wait(lk, [&] {
            if (stopping_) return true;
            if (queue_bytes_ < max_queue_bytes) return true;
            bool other_empty = pkt.track == vtrack_ ? (atrack_ && aq_.empty()) : vq_.empty();
            return other_empty && queue_bytes_ < max_queue_bytes * 2;
        });
        if (stopping_) return;
        queue_bytes_ += pkt.data.size();
        (pkt.track == vtrack_ ? vq_ : aq_).push_back(std::move(pkt));
        cv_.notify_all();
    }
}

void Player::audio_loop() {
    Mp3Decoder dec;
    StereoResampler rs;
    std::vector<int16_t> pcm, stereo;
    uint32_t out_rate = audio_.rate();
    std::vector<int16_t> silence(1024 * 2, 0);
    for (;;) {
        MkvPacket pkt;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || (!paused_ && (!aq_.empty() || eof_)); });
            if (stopping_) return;
            if (aq_.empty()) {  // eof: hand the clock over to the wall clock
                if (base_pts_ >= 0) {
                    uint64_t played_ms = audio_.played_frames() * 1000 / audio_.rate();
                    wall_start_ms_ = now_ms() - played_ms;
                    paused_total_ms_ = 0;
                }
                audio_done_ = true;
                cv_.notify_all();
                return;
            }
            pkt = std::move(aq_.front());
            aq_.pop_front();
            queue_bytes_ -= pkt.data.size();
            cv_.notify_all();
        }
        pcm.clear();
        int rate = 0, ch = 0;
        size_t n = dec.decode(pkt.data.data(), pkt.data.size(), pcm, rate, ch);
        if (!n) continue;
        stereo.clear();
        rs.process(pcm.data(), n, rate, ch, (int)out_rate, stereo);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (base_pts_ < 0) base_pts_ = pkt.pts_ms;
        }
        if (!audio_.write(stereo.data(), stereo.size() / 2)) {
            std::lock_guard<std::mutex> lk(mu_);
            error_ = "audio output failed";
            audio_done_ = true;
            return;
        }
    }
}

int64_t Player::clock_ms_locked() {
    if (base_pts_ < 0) return -1;
    if (atrack_ && !audio_done_) {
        return base_pts_ + (int64_t)(audio_.played_frames() * 1000 / audio_.rate());
    }
    if (!wall_start_ms_) return base_pts_;
    uint64_t now = paused_ ? paused_at_ms_ : now_ms();
    return base_pts_ + (int64_t)(now - wall_start_ms_ - paused_total_ms_);
}

void Player::set_paused(bool p) {
    std::lock_guard<std::mutex> lk(mu_);
    if (p == paused_) return;
    if (p) paused_at_ms_ = now_ms();
    else if (paused_at_ms_) paused_total_ms_ += now_ms() - paused_at_ms_;
    paused_ = p;
    cv_.notify_all();
}

int64_t Player::position_ms() {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t c = clock_ms_locked();
    if (c >= 0) last_pos_ms_ = c;
    return last_pos_ms_;
}

bool Player::finished() {
    std::lock_guard<std::mutex> lk(mu_);
    return eof_ && vq_.empty() && aq_.empty() && audio_done_;
}

std::string Player::error() {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

PlayerStats Player::stats() {
    std::lock_guard<std::mutex> lk(mu_);
    PlayerStats s = stats_;
    s.video_queue = (uint32_t)vq_.size();
    s.audio_queue = (uint32_t)aq_.size();
    s.queue_bytes = (uint32_t)queue_bytes_;
    s.avg_decode_us = stats_.frames_shown ? (uint32_t)(decode_us_total_ / stats_.frames_shown) : 0;
    return s;
}

int Player::video_tick() {
    MkvPacket pkt;
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!vtrack_ || vq_.empty()) {
            // Video-only stream that ended, or waiting for data.
            if (!atrack_ && eof_ && vq_.empty()) audio_done_ = true;
            return 10;
        }
        // No audio clock yet: show the first frame as a preview, then wait.
        if (base_pts_ < 0 && !atrack_) {
            base_pts_ = vq_.front().pts_ms;
            wall_start_ms_ = now_ms();
        }
        int64_t clock = clock_ms_locked();
        if (clock < 0) {
            if (first_frame_shown_) return 5;
        } else {
            if (paused_ && first_frame_shown_) return 20;
            if (vq_.front().pts_ms > clock + 5) {
                int64_t wait = vq_.front().pts_ms - clock;
                return wait > 20 ? 20 : (int)wait;
            }
            // Drop frames that are already superseded by a later due frame.
            while (vq_.size() > 1 && vq_[1].pts_ms <= clock) {
                queue_bytes_ -= vq_.front().data.size();
                vq_.pop_front();
                stats_.frames_dropped++;
            }
        }
        pkt = std::move(vq_.front());
        vq_.pop_front();
        queue_bytes_ -= pkt.data.size();
        cv_.notify_all();
    }
    uint64_t t0 = now_us();
    int w = 0, h = 0;
    bool ok = jpeg_decode_rgba(pkt.data.data(), pkt.data.size(), rgba_, w, h);
    uint32_t us = (uint32_t)(now_us() - t0);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ok) {
            stats_.decode_errors++;
            return 0;
        }
        first_frame_shown_ = true;
        stats_.frames_shown++;
        decode_us_total_ += us;
        if (us > stats_.max_decode_us) stats_.max_decode_us = us;
    }
    video_.present(rgba_.data(), w, h, pkt.pts_ms);  // may wait for vsync: keep it outside the lock
    return 0;
}

}  // namespace nxp
