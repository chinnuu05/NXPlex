// Picture-in-picture output for the sysmodule: a small vi layer on top of
// everything (drawn with the CPU, RGB565 to save memory) and an audout sink.
#pragma once
#include <switch.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "nxplex_ipc.h"
#include "platform.hpp"
#include "player.hpp"

extern "C" u64 __nx_vi_layer_id;  // libnx: viCreateLayer() adopts this managed layer

namespace nxp {

// ------------------------------------------------------------------ display

class PipDisplay : public VideoSink {
public:
    // The window exists while playback wants it (open/close) AND Tesla isn't
    // showing (set_hidden). The display hardware only shows one of our layer
    // and Tesla's, so the video steps aside while Tesla is open.
    void open() {
        std::lock_guard<std::mutex> lk(mu_);
        active_ = true;
        sync_locked();
    }
    void close() {
        std::lock_guard<std::mutex> lk(mu_);
        active_ = false;
        sync_locked();
        last_.clear();
        last_.shrink_to_fit();
        last_w_ = last_h_ = 0;
        paused_ = false;
    }
    bool is_open() {
        std::lock_guard<std::mutex> lk(mu_);
        return active_;
    }
    void set_hidden(bool hidden) {
        std::lock_guard<std::mutex> lk(mu_);
        if (hidden == hidden_) return;
        hidden_ = hidden;
        sync_locked();
    }

    void set_layout(int corner, int size) {
        std::lock_guard<std::mutex> lk(mu_);
        corner_ = corner & 3;
        size_ = size % 3;
        apply_rect_locked();
    }

    void present(const uint8_t* rgba, int w, int h, int64_t) override {
        std::lock_guard<std::mutex> lk(mu_);
        // Keep the frame (RGB565) for redraws while paused / after Tesla closes.
        last_.resize((size_t)w * h);
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
            const uint8_t* p = rgba + i * 4;
            last_[i] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
        last_w_ = w;
        last_h_ = h;
        frame_seq_++;
        if (!window_ok_) return;
        if (w != fb_w_ || h != fb_h_) {
            if (!make_fb_locked(w, h)) return;
            apply_rect_locked();  // aspect ratio may have changed
        }
        draw_locked();
    }

    // Copies the latest frame for the overlay's preview. Pixels are only
    // copied when the frame changed since `have_seq`.
    size_t copy_frame(uint32_t have_seq, void* out, size_t size) {
        std::lock_guard<std::mutex> lk(mu_);
        if (size < sizeof(NxpFrameHeader)) return 0;
        NxpFrameHeader hdr = {};
        size_t px = (size_t)last_w_ * last_h_ * 2;
        bool fits = !last_.empty() && last_w_ <= NXP_FRAME_MAX_W && last_h_ <= NXP_FRAME_MAX_H &&
                    sizeof(hdr) + px <= size;
        hdr.seq = fits ? frame_seq_ : 0;
        hdr.width = (uint16_t)last_w_;
        hdr.height = (uint16_t)last_h_;
        hdr.has_pixels = fits && frame_seq_ != have_seq;
        memcpy(out, &hdr, sizeof(hdr));
        if (hdr.has_pixels) memcpy((uint8_t*)out + sizeof(hdr), last_.data(), px);
        return sizeof(hdr) + (hdr.has_pixels ? px : 0);
    }

    // OSD state, drawn over the video.
    void set_paused(bool p) {
        std::lock_guard<std::mutex> lk(mu_);
        if (p == paused_) return;
        paused_ = p;
        draw_locked();
    }
    void set_progress(float f) {
        std::lock_guard<std::mutex> lk(mu_);
        progress_ = f;
    }
    void flash_progress(uint32_t ms) {
        std::lock_guard<std::mutex> lk(mu_);
        progress_until_ = now_ms() + ms;
    }
    void flash_volume(int v, uint32_t ms) {
        std::lock_guard<std::mutex> lk(mu_);
        volume_ = v / 100.0f;
        volume_until_ = now_ms() + ms;
    }
    // While paused nothing calls present(); redraw if a bar appeared/expired.
    void tick_osd() {
        std::lock_guard<std::mutex> lk(mu_);
        bool active = now_ms() < progress_until_ || now_ms() < volume_until_;
        if ((active || osd_was_active_) && paused_) draw_locked();
        osd_was_active_ = active;
    }

private:
    Result add_to_stack(ViLayerStack stack) {
        const struct {
            u32 stack;
            u64 layerId;
        } in = {stack, layer_.layer_id};
        return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
    }

    void sync_locked() {
        bool want = active_ && !hidden_;
        if (want && !layer_ok_) {
            if (create_layer_locked() && last_w_ > 0 && make_fb_locked(last_w_, last_h_)) {
                apply_rect_locked();
                draw_locked();  // show the last frame right away (matters when paused)
            }
        } else if (!want && layer_ok_) {
            destroy_layer_locked();
        }
    }

    bool create_layer_locked() {
        Result rc = viInitialize(ViServiceType_Manager);
        if (R_SUCCEEDED(rc)) rc = viOpenDefaultDisplay(&display_);
        if (R_SUCCEEDED(rc)) rc = viCreateManagedLayer(&display_, (ViLayerFlags)0, 0, &__nx_vi_layer_id);
        if (R_SUCCEEDED(rc)) rc = viCreateLayer(&display_, &layer_);
        if (R_SUCCEEDED(rc)) rc = viSetLayerScalingMode(&layer_, ViScalingMode_FitToLayer);
        if (R_FAILED(rc)) {
            logf("display: vi setup failed 0x%x", rc);
            viExit();
            return false;
        }
        s32 z = 0;
        if (R_SUCCEEDED(viGetZOrderCountMax(&display_, &z)) && z > 1) viSetLayerZ(&layer_, z - 1);
        const ViLayerStack stacks[] = {ViLayerStack_Default, ViLayerStack_Screenshot, ViLayerStack_Recording,
                                       ViLayerStack_Arbitrary, ViLayerStack_LastFrame, ViLayerStack_Null,
                                       ViLayerStack_ApplicationForDebug, ViLayerStack_Lcd};
        for (auto s : stacks) add_to_stack(s);
        layer_ok_ = true;
        apply_rect_locked();
        rc = nwindowCreateFromLayer(&window_, &layer_);
        if (R_FAILED(rc)) {
            logf("display: nwindow failed 0x%x", rc);
            destroy_layer_locked();
            return false;
        }
        window_ok_ = true;
        logf("display: layer created");
        return true;
    }

    void destroy_layer_locked() {
        if (fb_ok_) framebufferClose(&fb_);
        if (window_ok_) nwindowClose(&window_);
        if (layer_ok_) {
            viDestroyManagedLayer(&layer_);
            viCloseDisplay(&display_);
            viExit();
            logf("display: layer removed");
        }
        fb_ok_ = window_ok_ = layer_ok_ = false;
        fb_w_ = fb_h_ = 0;
    }

    bool make_fb_locked(int w, int h) {
        if (fb_ok_) framebufferClose(&fb_);
        fb_ok_ = false;
        Result rc = framebufferCreate(&fb_, &window_, (u32)w, (u32)h, PIXEL_FORMAT_RGB_565, 2);
        if (R_SUCCEEDED(rc)) rc = framebufferMakeLinear(&fb_);
        if (R_FAILED(rc)) {
            logf("display: framebuffer %dx%d failed 0x%x", w, h, rc);
            return false;
        }
        fb_w_ = w;
        fb_h_ = h;
        fb_ok_ = true;
        return true;
    }

    // Window box by size preset (1920x1080 layer space), video fitted inside
    // keeping its aspect ratio and anchored to the chosen corner.
    void apply_rect_locked() {
        if (!layer_ok_) return;
        static const int widths[3] = {384, 512, 768};
        int bw = widths[size_], bh = bw * 9 / 16, m = 24;
        int w = bw, h = bh;
        int fw = fb_w_ ? fb_w_ : last_w_, fh = fb_h_ ? fb_h_ : last_h_;
        if (fw > 0 && fh > 0) {
            if (fw * bh > fh * bw) h = bw * fh / fw;  // wider than 16:9
            else w = bh * fw / fh;                    // narrower (e.g. 4:3)
        }
        bool right = corner_ == 0 || corner_ == 3, top = corner_ <= 1;
        float x = right ? 1920 - w - m : m;
        float y = top ? m : 1080 - h - m;
        viSetLayerSize(&layer_, w, h);
        viSetLayerPosition(&layer_, x, y);
    }

    void draw_locked() {
        if (!fb_ok_ || last_.empty() || last_w_ != fb_w_ || last_h_ != fb_h_) return;
        u32 stride = 0;
        uint8_t* dst = (uint8_t*)framebufferBegin(&fb_, &stride);
        for (int y = 0; y < fb_h_; y++)
            memcpy(dst + (size_t)y * stride, last_.data() + (size_t)y * fb_w_, (size_t)fb_w_ * 2);
        auto rect = [&](int x0, int y0, int x1, int y1, uint16_t c) {
            for (int y = y0 < 0 ? 0 : y0; y < y1 && y < fb_h_; y++) {
                uint16_t* row = (uint16_t*)(dst + (size_t)y * stride);
                for (int x = x0 < 0 ? 0 : x0; x < x1 && x < fb_w_; x++) row[x] = c;
            }
        };
        const uint16_t kGrey = 0x4208, kWhite = 0xFFFF, kAmber = 0xE501;  // Plex amber #E5A00D
        uint64_t now = now_ms();
        if ((paused_ || now < progress_until_) && progress_ >= 0) {
            int bar_h = fb_h_ / 36 + 2, y0 = fb_h_ - bar_h - 3;
            int filled = (int)(progress_ * (float)(fb_w_ - 8));
            rect(4, y0, fb_w_ - 4, y0 + bar_h, kGrey);
            rect(4, y0, 4 + filled, y0 + bar_h, kAmber);
        }
        if (now < volume_until_) {
            int bar_w = fb_w_ / 48 + 2, x0 = fb_w_ - bar_w - 4, top = fb_h_ / 8, bot = fb_h_ - fb_h_ / 8;
            int lvl = bot - (int)(volume_ * (float)(bot - top));
            rect(x0, top, x0 + bar_w, bot, kGrey);
            rect(x0, lvl, x0 + bar_w, bot, kWhite);
        }
        if (paused_) {
            int bh = fb_h_ / 4, bw = fb_w_ / 40 + 2, cx = fb_w_ / 2, cy = fb_h_ / 2;
            rect(cx - 2 * bw, cy - bh / 2, cx - bw / 2, cy + bh / 2, kWhite);
            rect(cx + bw / 2, cy - bh / 2, cx + 2 * bw, cy + bh / 2, kWhite);
        }
        framebufferEnd(&fb_);
    }

    std::mutex mu_;
    ViDisplay display_ = {};
    ViLayer layer_ = {};
    NWindow window_ = {};
    Framebuffer fb_ = {};
    bool active_ = false, hidden_ = false;
    bool layer_ok_ = false, window_ok_ = false, fb_ok_ = false;
    int fb_w_ = 0, fb_h_ = 0, last_w_ = 0, last_h_ = 0;
    int corner_ = 0, size_ = 1;
    bool paused_ = false, osd_was_active_ = false;
    float progress_ = -1, volume_ = 1;
    uint64_t progress_until_ = 0, volume_until_ = 0;
    std::vector<uint16_t> last_;
    uint32_t frame_seq_ = 0;
};

// ------------------------------------------------------------------ audio

class SwitchAudio : public AudioSink {
public:
    bool init() {  // needs sm
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) {
            logf("audio: audoutInitialize failed 0x%x", rc);
            return false;
        }
        inited_ = true;
        return true;
    }

    bool open(uint32_t) override {
        if (!inited_) return false;
        reset_state();
        Result rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            logf("audio: start failed 0x%x", rc);
            return false;
        }
        started_ = true;
        apply_volume();
        return true;
    }

    uint32_t rate() const override { return 48000; }

    bool write(const int16_t* pcm, size_t frames) override {
        while (frames) {
            while (busy_[fill_]) reap(true);
            size_t space = kFrames - fill_frames_;
            size_t n = frames < space ? frames : space;
            memcpy((int16_t*)mem_[fill_] + fill_frames_ * 2, pcm, n * 4);
            fill_frames_ += n;
            pcm += n * 2;
            frames -= n;
            if (fill_frames_ == kFrames) submit();
            reap(false);
        }
        return true;
    }

    uint64_t played_frames() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inflight_) return released_frames_;
        uint64_t since = (now_us() - head_start_us_) * 48000 / 1000000;
        if (since > kFrames) since = kFrames;
        return released_frames_ + since;
    }

    void flush() override {
        if (!started_) return;
        stop_and_drain();
        audoutStartAudioOut();
        started_ = true;
        apply_volume();
    }

    void close() override {
        if (started_) stop_and_drain();
    }

    void exit() {
        close();
        if (inited_) audoutExit();
        inited_ = false;
    }

    void set_volume(int v) {
        volume_ = v / 100.0f;
        apply_volume();
    }

private:
    static const int kBuffers = 4;
    static const size_t kFrames = 2048;  // 42.7 ms per buffer

    void apply_volume() {
        if (started_ && hosversionAtLeast(6, 0, 0)) audoutSetAudioOutVolume(volume_);
    }

    void reset_state() {
        for (int i = 0; i < kBuffers; i++) {
            bufs_[i] = {};
            bufs_[i].buffer = mem_[i];
            bufs_[i].buffer_size = sizeof(mem_[i]);
            busy_[i] = false;
        }
        fill_ = 0;
        fill_frames_ = 0;
        std::lock_guard<std::mutex> lk(mu_);
        released_frames_ = 0;
        inflight_ = 0;
    }

    // Stops output and makes the service hand back every queued buffer.
    void stop_and_drain() {
        audoutStopAudioOut();
        bool flushed = false;
        if (hosversionAtLeast(4, 0, 0)) audoutFlushAudioOutBuffers(&flushed);
        for (int guard = 0; guard < 16; guard++) {
            AudioOutBuffer* rel = nullptr;
            u32 count = 0;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &count)) || !count) break;
        }
        started_ = false;
        reset_state();
    }

    void submit() {
        bufs_[fill_].data_size = fill_frames_ * 4;
        bufs_[fill_].data_offset = 0;
        if (R_FAILED(audoutAppendAudioOutBuffer(&bufs_[fill_]))) {
            fill_frames_ = 0;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (inflight_ == 0) head_start_us_ = now_us();
            inflight_++;
        }
        busy_[fill_] = true;
        fill_ = (fill_ + 1) % kBuffers;
        fill_frames_ = 0;
    }

    void reap(bool block) {
        AudioOutBuffer* rel = nullptr;
        u32 count = 0;
        Result rc = block ? audoutWaitPlayFinish(&rel, &count, 200'000'000ULL)
                          : audoutGetReleasedAudioOutBuffer(&rel, &count);
        if (R_FAILED(rc)) {
            // Timed out (e.g. output stalled in sleep mode). If nothing is in
            // flight, free the buffer we're waiting on so we can't deadlock.
            std::lock_guard<std::mutex> lk(mu_);
            if (block && inflight_ == 0) busy_[fill_] = false;
            return;
        }
        while (count > 0 && rel) {
            for (int i = 0; i < kBuffers; i++) {
                if (rel == &bufs_[i] && busy_[i]) {
                    busy_[i] = false;
                    std::lock_guard<std::mutex> lk(mu_);
                    released_frames_ += rel->data_size / 4;
                    if (inflight_) inflight_--;
                    head_start_us_ = now_us();
                }
            }
            rel = nullptr;
            count = 0;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &count))) break;
        }
    }

    alignas(0x1000) int16_t mem_[kBuffers][kFrames * 2];
    AudioOutBuffer bufs_[kBuffers] = {};
    bool busy_[kBuffers] = {};
    int fill_ = 0;
    size_t fill_frames_ = 0;
    std::mutex mu_;
    uint64_t released_frames_ = 0, head_start_us_ = 0;
    int inflight_ = 0;
    bool inited_ = false, started_ = false;
    float volume_ = 1.0f;
};

}  // namespace nxp
