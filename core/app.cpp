#include "app.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform.hpp"

namespace nxp {

namespace {

const char* kResolution[3] = {"256x144", "384x216", "512x288"};
const int kMaxBitrate[3] = {1000, 2000, 3000};

// Plex transcodes anything to this (see README "How it works").
const char* kProfileExtra =
    "add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mkv&videoCodec=mjpeg&audioCodec=mp3)"
    "+add-limitation(scope=videoAudioCodec&scopeName=mp3&type=upperBound&name=audio.channels&value=2)";

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string random_hex(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    random_bytes(b.data(), b.size());
    std::string s;
    char h[3];
    for (uint8_t x : b) {
        snprintf(h, sizeof(h), "%02x", x);
        s += h;
    }
    return s;
}

// Logs end up in bug reports: strip account/server-identifying details.
// "https://192-168-1-5.<hash>.plex.direct:32400" -> "https://192-168-1-5.<id>.plex.direct:32400";
// public addresses are hidden entirely, private LAN addresses are kept.
std::string redact(std::string s) {
    size_t pd = s.find(".plex.direct");
    while (pd != std::string::npos) {
        size_t start = s.rfind('/', pd);
        start = start == std::string::npos ? 0 : start + 1;
        std::string host = s.substr(start, pd - start);  // "<ip>.<hash>"
        size_t dot = host.find('.');
        std::string ip = dot == std::string::npos ? "" : host.substr(0, dot);
        bool lan = ip.rfind("192-168-", 0) == 0 || ip.rfind("10-", 0) == 0 || ip.rfind("172-", 0) == 0;
        std::string repl = (lan ? ip : std::string("<public-ip>")) + ".<id>";
        s.replace(start, pd - start, repl);
        pd = s.find(".plex.direct", start + repl.size() + 12);
    }
    size_t tok = s.find("X-Plex-Token=");
    if (tok != std::string::npos) s.replace(tok + 13, s.find('&', tok) == std::string::npos ? std::string::npos : s.find('&', tok) - tok - 13, "<redacted>");
    return s;
}

std::string fmt_episode(const PlexItem& it) {
    char b[160];
    if (it.parent_index >= 0 && it.index >= 0)
        snprintf(b, sizeof(b), "S%d E%d - %s", it.parent_index, it.index, it.title.c_str());
    else
        snprintf(b, sizeof(b), "%s", it.title.c_str());
    return b;
}

void to_ipc_item(const PlexItem& it, NxpItem& o) {
    memset(&o, 0, sizeof(o));
    o.duration_ms = it.duration_ms;
    o.view_offset_ms = it.view_offset_ms;
    o.leaf_count = it.leaf_count;
    o.viewed_leaf_count = it.viewed_leaf_count;
    o.index = (int16_t)it.index;
    o.parent_index = (int16_t)it.parent_index;
    o.year = (uint16_t)(it.year > 0 ? it.year : 0);
    o.is_leaf = it.is_leaf() ? 1 : 0;
    o.is_section = it.is_section ? 1 : 0;
    copy_str(o.type, it.type.c_str());
    copy_str(o.title, it.title.c_str());
    copy_str(o.parent, it.parent.c_str());
    copy_str(o.grandparent, it.grandparent.c_str());
    copy_str(o.key, it.key.c_str());
}

}  // namespace

// ------------------------------------------------------------------ setup

App::App(Config& cfg, const PlexClient& client, AudioSink& audio, VideoSink& video, Hooks hooks)
    : cfg_(cfg), client_(client), server_cl_(client), audio_(audio), video_(video), hooks_(std::move(hooks)) {
    server_cl_.platform = "Generic";
    browse_ = new NxpBrowse();
    memset(browse_, 0, sizeof(*browse_));
    client_.user_token = cfg_.get("user_token");
    account_ = cfg_.get("account");
    auth_ = client_.user_token.empty() ? NxpAuth_SignedOut : NxpAuth_SignedIn;
    volume_ = clampi(atoi(cfg_.get("volume", "100").c_str()), 0, 100);
    corner_ = clampi(atoi(cfg_.get("pip_corner", "0").c_str()), 0, 3);
    size_ = clampi(atoi(cfg_.get("pip_size", "1").c_str()), 0, 2);
    quality_ = clampi(atoi(cfg_.get("quality", "1").c_str()), 0, 2);
}

App::~App() {
    stop();
    delete browse_;
}

bool App::start() {
    if (hooks_.set_volume) hooks_.set_volume(volume_);
    if (hooks_.set_layout) hooks_.set_layout(corner_, size_);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        running_ = true;
        // Signed in but never picked a server: look them up right away.
        if (auth_ == NxpAuth_SignedIn && cfg_.get("server_url").empty()) jobs_.push_back(Job::Servers);
    }
    return worker_.start(worker_entry, this, 0x20000, 0x3B);  // below Tesla's UI threads
}

void App::stop() {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    worker_.join();
    report_timeline("stopped");
    teardown_player();
    std::lock_guard<std::mutex> lk(state_mu_);
    if (cfg_dirty_) save_config_locked();
}

void App::save_config_locked() {
    cfg_.set("volume", std::to_string(volume_));
    cfg_.set("pip_corner", std::to_string(corner_));
    cfg_.set("pip_size", std::to_string(size_));
    cfg_.set("quality", std::to_string(quality_));
    if (!cfg_.save()) logf("app: could not save %s", cfg_.path().c_str());
    cfg_dirty_ = false;
}

void App::set_error(const std::string& e) {
    error_ = e;
    if (!e.empty()) logf("app: %s", e.c_str());
}

void App::queue(Job j) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        bool dup = false;
        for (Job q : jobs_) dup = dup || q == j;
        if (!dup) jobs_.push_back(j);
    }
    cv_.notify_all();
}

PlexServerSession App::session() {
    PlexServerSession s;
    std::lock_guard<std::mutex> lk(state_mu_);
    s.base_url = cfg_.get("server_url");
    s.token = cfg_.get("server_token");
    return s;
}

void App::wait_idle(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(state_mu_);
    uint64_t end = now_ms() + timeout_ms;
    while ((!jobs_.empty() || busy_) && now_ms() < end) cv_.wait_for(lk, std::chrono::milliseconds(50));
}

// ------------------------------------------------------------------ queries

void App::get_status(NxpStatus& o) {
    memset(&o, 0, sizeof(o));
    uint32_t pos;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        pos = position_locked();
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    o.api_version = NXP_API_VERSION;
    o.auth_state = auth_;
    o.play_state = play_state_;
    o.browse_state = browse_->state;
    o.servers_state = servers_state_;
    o.volume = (uint8_t)volume_;
    o.corner = (uint8_t)corner_;
    o.size = (uint8_t)size_;
    o.quality = (uint8_t)quality_;
    o.has_server = cfg_.get("server_url").empty() ? 0 : 1;
    o.browse_seq = browse_->seq;
    o.position_ms = pending_seek_ms_ >= 0 ? (uint32_t)pending_seek_ms_ : pos;
    o.duration_ms = duration_ms_;
    copy_str(o.pin_code, pin_.code.c_str());
    copy_str(o.account, account_.c_str());
    copy_str(o.server_name, cfg_.get("server_name").c_str());
    copy_str(o.title, title_.c_str());
    copy_str(o.subtitle, subtitle_.c_str());
    copy_str(o.error, error_.c_str());
}

void App::get_servers(NxpServers& o) {
    memset(&o, 0, sizeof(o));
    std::lock_guard<std::mutex> lk(state_mu_);
    std::string sel = cfg_.get("server_id");
    for (auto& s : servers_) {
        if (o.count >= NXP_MAX_SERVERS) break;
        NxpServerInfo& i = o.servers[o.count++];
        copy_str(i.name, s.name.c_str());
        i.owned = s.owned;
        i.online = s.presence;
        i.selected = s.machine_id == sel;
    }
    copy_str(o.error, servers_error_.c_str());
}

void App::get_browse(NxpBrowse& o) {
    std::lock_guard<std::mutex> lk(state_mu_);
    memcpy(&o, browse_, sizeof(o));
}

bool App::quit_requested() {
    std::lock_guard<std::mutex> lk(state_mu_);
    return quit_;
}

bool App::playing_or_loading() {
    std::lock_guard<std::mutex> lk(state_mu_);
    return play_state_ != NxpPlay_Stopped;
}

// ------------------------------------------------------------------ commands

void App::cmd_start_login() {
    // The PIN itself is created by the worker (network); see periodic().
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        error_.clear();
        pin_ = PlexPin();
        auth_ = NxpAuth_PinPending;
        pin_next_poll_ = 0;
        pin_deadline_ = 0;
    }
    cv_.notify_all();
}

void App::cmd_logout() {
    queue(Job::Stop);
    std::lock_guard<std::mutex> lk(state_mu_);
    auth_ = NxpAuth_SignedOut;
    pin_ = PlexPin();
    client_.user_token.clear();
    account_.clear();
    servers_.clear();
    servers_state_ = NxpServers_Idle;
    for (const char* k : {"user_token", "account", "server_url", "server_token", "server_name", "server_id"})
        cfg_.erase(k);
    save_config_locked();
    memset(browse_, 0, sizeof(*browse_));
}

void App::cmd_refresh_servers() {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        servers_state_ = NxpServers_Loading;
        servers_error_.clear();
    }
    queue(Job::Servers);
}

void App::cmd_select_server(uint32_t index) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (index >= servers_.size()) return;
        select_index_ = index;
        servers_state_ = NxpServers_Loading;
        error_.clear();
    }
    queue(Job::SelectServer);
}

uint32_t App::cmd_browse(const std::string& path, uint32_t start) {
    uint32_t seq;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        seq = ++browse_seq_;
        browse_path_ = path;
        browse_start_ = start;
        memset(browse_, 0, sizeof(*browse_));
        browse_->seq = seq;
        browse_->state = NxpBrowse_Loading;
        browse_->offset = start;
        copy_str(browse_->path, path.c_str());
    }
    queue(Job::Browse);
    return seq;
}

void App::cmd_play(const std::string& key, bool from_start) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        play_key_ = key;
        play_from_start_ = from_start;
        pending_seek_ms_ = -1;
        play_state_ = NxpPlay_Loading;
        error_.clear();
    }
    queue(Job::Play);
}

void App::cmd_play_file(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        play_file_ = path;
        pending_seek_ms_ = -1;
        play_state_ = NxpPlay_Loading;
        error_.clear();
    }
    queue(Job::PlayFile);
}

void App::cmd_set_paused(int mode) {
    bool now_paused = false, changed = false;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        if (!player_) return;
        bool p = mode == 2 ? !player_->paused() : mode == 1;
        changed = p != player_->paused();
        player_->set_paused(p);
        now_paused = p;
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    if (play_state_ == NxpPlay_Playing || play_state_ == NxpPlay_Paused)
        play_state_ = now_paused ? NxpPlay_Paused : NxpPlay_Playing;
    if (changed) next_timeline_ms_ = 0;  // report right away
    cv_.notify_all();
}

void App::cmd_seek_relative(int32_t delta_ms) {
    uint32_t pos;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        if (!player_) return;
        pos = position_locked();
    }
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        int64_t base = pending_seek_ms_ >= 0 ? pending_seek_ms_ : (int64_t)pos;
        int64_t t = base + delta_ms;
        if (t < 0) t = 0;
        if (duration_ms_ && t > (int64_t)duration_ms_ - 5000) t = duration_ms_ > 5000 ? duration_ms_ - 5000 : 0;
        pending_seek_ms_ = t;
    }
    queue(Job::Seek);
}

void App::cmd_seek_absolute(uint32_t ms) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (play_state_ == NxpPlay_Stopped) return;
        pending_seek_ms_ = ms;
    }
    queue(Job::Seek);
}

void App::cmd_stop() { queue(Job::Stop); }

void App::cmd_set_volume(int v) {
    v = clampi(v, 0, 100);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        volume_ = v;
        cfg_dirty_ = true;
    }
    if (hooks_.set_volume) hooks_.set_volume(v);
}

void App::cmd_set_layout(int corner, int size) {
    corner = clampi(corner, 0, 3);
    size = clampi(size, 0, 2);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        corner_ = corner;
        size_ = size;
        cfg_dirty_ = true;
    }
    if (hooks_.set_layout) hooks_.set_layout(corner, size);
}

void App::cmd_set_quality(int q) {
    bool restart;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        q = clampi(q, 0, 2);
        if (q == quality_) return;
        quality_ = q;
        cfg_dirty_ = true;
        restart = play_state_ != NxpPlay_Stopped;
    }
    bool file;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        file = is_file_;
    }
    if (restart && !file) queue(Job::Restart);
}

void App::cmd_quit() {
    std::lock_guard<std::mutex> lk(state_mu_);
    quit_ = true;
}

// ------------------------------------------------------------------ display thread

int App::video_tick(bool& active) {
    std::lock_guard<std::mutex> lk(play_mu_);
    if (!player_) {
        active = false;
        return 20;
    }
    active = true;
    int wait = player_->video_tick();
    if (!finished_flag_ && player_->finished()) {
        finished_flag_ = true;
        cv_.notify_all();  // worker picks it up in periodic()
    }
    return wait;
}

uint32_t App::position_locked() {
    if (!player_) return stream_offset_ms_;
    int64_t p = player_->position_ms();
    if (is_file_) return (uint32_t)(p < 0 ? 0 : p);
    return stream_offset_ms_ + (uint32_t)(p < 0 ? 0 : p);
}

// ------------------------------------------------------------------ worker

void App::worker_loop() {
    std::unique_lock<std::mutex> lk(state_mu_);
    while (running_) {
        if (!jobs_.empty()) {
            Job j = jobs_.front();
            jobs_.erase(jobs_.begin());
            busy_ = true;
            lk.unlock();
            run_job(j);
            lk.lock();
            busy_ = false;
            cv_.notify_all();
            continue;
        }
        lk.unlock();
        periodic();
        lk.lock();
        if (jobs_.empty() && running_) cv_.wait_for(lk, std::chrono::milliseconds(250));
    }
}

void App::run_job(Job j) {
    switch (j) {
    case Job::Servers: job_servers(); break;
    case Job::SelectServer: job_select_server(); break;
    case Job::Browse: job_browse(); break;
    case Job::Play: job_play(); break;
    case Job::PlayFile: job_play_file(); break;
    case Job::Seek: job_seek(); break;
    case Job::Stop: job_stop(true, "stopped"); break;
    case Job::Restart: job_restart(); break;
    case Job::None: break;
    }
}

void App::periodic() {
    uint64_t now = now_ms();

    // --- sign-in: create the PIN, then poll it.
    bool need_pin = false, poll_pin = false;
    PlexPin pin;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (auth_ == NxpAuth_PinPending) {
            if (pin_.id == 0 && now >= pin_next_poll_) need_pin = true;
            else if (pin_.id && now >= pin_next_poll_) poll_pin = true;
            pin = pin_;
        }
    }
    if (need_pin) {
        std::string err;
        PlexPin p;
        bool ok = pin_create(client_, p, err);
        std::lock_guard<std::mutex> lk(state_mu_);
        if (auth_ == NxpAuth_PinPending) {
            if (ok) {
                pin_ = p;
                pin_deadline_ = now_ms() + (uint64_t)(p.expires_in_s ? p.expires_in_s : 300) * 1000;
                pin_next_poll_ = now_ms() + 2000;
                logf("app: sign-in code %s", p.code.c_str());
            } else {
                auth_ = NxpAuth_SignedOut;
                set_error("Sign-in failed: " + err);
            }
        }
    } else if (poll_pin) {
        std::string token, err;
        PinStatus st = pin_check(client_, pin, token, err);
        std::string name;
        if (st == PinStatus::Linked && !account_name(client_, token, name, err)) st = PinStatus::Error;
        std::lock_guard<std::mutex> lk(state_mu_);
        if (auth_ == NxpAuth_PinPending && pin_.id == pin.id) {
            pin_next_poll_ = now_ms() + 2000;
            if (st == PinStatus::Linked) {
                auth_ = NxpAuth_SignedIn;
                client_.user_token = token;
                account_ = name;
                pin_ = PlexPin();
                cfg_.set("user_token", token);
                cfg_.set("account", name);
                save_config_locked();
                logf("app: signed in");
                jobs_.push_back(Job::Servers);
            } else if (st == PinStatus::Expired || now_ms() > pin_deadline_) {
                auth_ = NxpAuth_SignedOut;
                pin_ = PlexPin();
                set_error("The sign-in code expired. Try again.");
            }
        }
    }

    // --- end of stream / stream errors.
    bool finished;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        finished = finished_flag_ && player_;
    }
    if (finished) {
        std::string perr;
        uint32_t pos, dur;
        bool file;
        {
            std::lock_guard<std::mutex> lk(play_mu_);
            perr = player_->error();
            pos = position_locked();
            file = is_file_;
        }
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            dur = duration_ms_;
            if (now - restart_window_ms_ > 60000) {
                restart_window_ms_ = now;
                restarts_ = 0;
            }
        }
        bool near_end = !dur || pos + 15000 >= dur;
        if (!file && !near_end && restarts_ < 3) {
            restarts_++;
            logf("app: stream ended early at %u ms (%s); reconnecting", pos, perr.empty() ? "no error" : perr.c_str());
            job_restart();
        } else {
            job_stop(true, near_end ? "finished" : "stream error");
            if (!near_end) {
                std::lock_guard<std::mutex> lk(state_mu_);
                set_error("Playback stopped: " + (perr.empty() ? std::string("stream ended") : perr));
            }
        }
    }

    // --- Plex timeline (resume position / watched state), every 10 s.
    bool report = false;
    const char* state = "playing";
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if ((play_state_ == NxpPlay_Playing || play_state_ == NxpPlay_Paused) && now >= next_timeline_ms_) {
            report = true;
            state = play_state_ == NxpPlay_Paused ? "paused" : "playing";
            next_timeline_ms_ = now + 10000;
        }
    }
    if (report) report_timeline(state);

    std::lock_guard<std::mutex> lk(state_mu_);
    if (cfg_dirty_) save_config_locked();
}

void App::report_timeline(const char* state) {
    std::string rating_key, key;
    uint32_t pos, dur;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        if (is_file_ || (!player_ && strcmp(state, "stopped") != 0)) return;
        pos = position_locked();
    }
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        rating_key = item_.rating_key;
        key = item_.key;
        dur = duration_ms_;
    }
    if (rating_key.empty()) return;
    PlexServerSession s = session();
    if (s.base_url.empty()) return;
    std::string path = "/:/timeline?ratingKey=" + rating_key + "&key=" + url_encode(key) + "&state=" + state +
                       "&time=" + std::to_string(pos) + "&duration=" + std::to_string(dur);
    HttpRequest req = server_request(server_cl_, s, path);
    req.timeout_ms = 5000;
    HttpResponse resp;
    http_fetch(req, resp, nullptr);
    if (resp.status != 200) logf("app: timeline %s -> %d %s", state, resp.status, resp.error.c_str());
}

void App::job_servers() {
    std::vector<PlexServer> list;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        servers_state_ = NxpServers_Loading;
    }
    bool ok = list_servers(client_, list, err);
    bool auto_select = false;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (!ok) {
            servers_state_ = NxpServers_Error;
            servers_error_ = err;
            set_error("Could not list servers: " + err);
            return;
        }
        servers_ = list;
        servers_state_ = NxpServers_Ready;
        servers_error_.clear();
        // Only one server on the account and none chosen yet: choose it.
        if (cfg_.get("server_url").empty() && servers_.size() == 1) {
            select_index_ = 0;
            auto_select = true;
        }
    }
    if (auto_select) job_select_server();
}

void App::job_select_server() {
    PlexServer s;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (select_index_ >= servers_.size()) return;
        s = servers_[select_index_];
        servers_state_ = NxpServers_Loading;
    }
    std::string base, err;
    std::vector<std::string> log;
    bool ok = pick_connection(client_, s, base, err, &log);
    for (auto& l : log) logf("app: probe %s", redact(l).c_str());
    std::lock_guard<std::mutex> lk(state_mu_);
    servers_state_ = ok ? NxpServers_Ready : NxpServers_Error;
    if (!ok) {
        servers_error_ = err;
        set_error("Can't reach " + s.name + ": " + err);
        return;
    }
    cfg_.set("server_id", s.machine_id);
    cfg_.set("server_name", s.name);
    cfg_.set("server_url", base);
    cfg_.set("server_token", s.access_token);
    save_config_locked();
    logf("app: server selected (%s)", redact(base).c_str());
}

void App::job_browse() {
    std::string path;
    uint32_t start, seq;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        path = browse_path_;
        start = browse_start_;
        seq = browse_seq_;
    }
    PlexServerSession s = session();
    PlexPage page;
    std::string err;
    bool ok = !s.base_url.empty();
    if (!ok) err = "No server selected";
    else ok = browse(server_cl_, s, path, start, NXP_MAX_ITEMS, page, err);

    std::lock_guard<std::mutex> lk(state_mu_);
    if (seq != browse_seq_) return;  // superseded
    if (!ok) {
        browse_->state = NxpBrowse_Error;
        copy_str(browse_->error, err.c_str());
        return;
    }
    browse_->offset = page.offset;
    browse_->total = page.total;
    browse_->count = 0;
    copy_str(browse_->title, page.title.c_str());
    for (auto& it : page.items) {
        if (browse_->count >= NXP_MAX_ITEMS) break;
        to_ipc_item(it, browse_->items[browse_->count++]);
    }
    browse_->state = NxpBrowse_Ready;
}

void App::teardown_player() {
    std::unique_ptr<Player> p;
    std::unique_ptr<HttpSource> h;
    std::unique_ptr<FileSource> f;
    std::string session_id;
    bool file;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        p = std::move(player_);
        h = std::move(http_);
        f = std::move(file_);
        session_id = transcode_session_;
        transcode_session_.clear();
        file = is_file_;
        finished_flag_ = false;
    }
    if (p) p->stop();
    p.reset();
    h.reset();
    f.reset();
    if (!session_id.empty() && !file) {
        PlexServerSession s = session();
        HttpRequest req = server_request(server_cl_, s, "/video/:/transcode/universal/stop?session=" + session_id);
        req.timeout_ms = 3000;
        HttpResponse resp;
        http_fetch(req, resp, nullptr);
    }
}

bool App::open_stream(uint32_t offset_ms, std::string& err) {
    std::string key;
    int q;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        key = item_.key;
        q = quality_;
    }
    PlexServerSession s = session();
    std::string sid = random_hex(12);
    std::string query = "path=" + url_encode(key) +
                        "&mediaIndex=0&partIndex=0&protocol=http&directPlay=0&directStream=0&fastSeek=1" +
                        "&videoResolution=" + kResolution[q] + "&maxVideoBitrate=" + std::to_string(kMaxBitrate[q]) +
                        "&audioBitrate=128&subtitles=none&offset=" + std::to_string(offset_ms / 1000) +
                        "&session=" + sid + "&X-Plex-Session-Identifier=" + sid +
                        "&X-Plex-Client-Profile-Extra=" + url_encode(kProfileExtra);
    if (!transcode_decision(server_cl_, s, query, err)) return false;
    std::string path = "/video/:/transcode/universal/start?" + query;
    auto http = std::unique_ptr<HttpSource>(new HttpSource());
    HttpRequest req = server_request(server_cl_, s, path);
    req.timeout_ms = 30000;
    HttpResponse resp;
    if (!http->open(req, resp)) {
        err = resp.error;
        return false;
    }
    if (resp.status != 200) {
        char body[200] = {0};
        long n = http->read(body, sizeof(body) - 1);
        logf("app: transcode start HTTP %d: %s", resp.status, n > 0 ? body : "");
        logf("app: request was %s", redact(path).c_str());
        err = "server refused the stream (HTTP " + std::to_string(resp.status) + ")";
        return false;
    }
    auto player = std::unique_ptr<Player>(new Player(*http, audio_, video_));
    player->max_queue_bytes = 1024 * 1024;
    if (!player->start(err)) return false;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        http_ = std::move(http);
        player_ = std::move(player);
        stream_offset_ms_ = (offset_ms / 1000) * 1000;
        is_file_ = false;
        transcode_session_ = sid;
        finished_flag_ = false;
    }
    logf("app: streaming %s from %u ms at %s", key.c_str(), offset_ms, kResolution[q]);
    return true;
}

void App::job_play() {
    std::string key;
    bool from_start;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        key = play_key_;
        from_start = play_from_start_;
    }
    report_timeline("stopped");
    teardown_player();

    PlexServerSession s = session();
    PlexPage page;
    std::string err;
    bool ok = !s.base_url.empty() && browse(server_cl_, s, key, 0, 1, page, err);
    if (ok && (page.items.empty() || !page.items[0].is_leaf())) {
        ok = false;
        err = "that item can't be played";
    }
    if (!ok) {
        std::lock_guard<std::mutex> lk(state_mu_);
        play_state_ = NxpPlay_Stopped;
        set_error("Can't play: " + (err.empty() ? std::string("no server") : err));
        return;
    }
    PlexItem it = page.items[0];
    it.key = key;  // /library/metadata/N (browse() may append /children for folders)
    uint32_t offset = from_start ? 0 : it.view_offset_ms;
    if (it.duration_ms && offset + 30000 > it.duration_ms) offset = 0;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        item_ = it;
        duration_ms_ = it.duration_ms;
        if (it.type == "episode") {
            title_ = it.grandparent;
            subtitle_ = fmt_episode(it);
        } else {
            title_ = it.title;
            subtitle_ = it.year > 0 ? std::to_string(it.year) : "";
        }
        restarts_ = 0;
        stream_offset_ms_ = offset;
    }
    if (!open_stream(offset, err)) {
        std::lock_guard<std::mutex> lk(state_mu_);
        play_state_ = NxpPlay_Stopped;
        set_error("Can't play: " + err);
        return;
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    play_state_ = NxpPlay_Playing;
    next_timeline_ms_ = 0;
}

void App::job_play_file() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        path = play_file_;
    }
    report_timeline("stopped");
    teardown_player();
    auto f = std::unique_ptr<FileSource>(new FileSource());
    std::string err;
    bool ok = f->open(path);
    std::unique_ptr<Player> player;
    if (!ok) err = "cannot open " + path;
    else {
        player.reset(new Player(*f, audio_, video_));
        ok = player->start(err);
    }
    std::lock_guard<std::mutex> lk(play_mu_);
    std::lock_guard<std::mutex> lk2(state_mu_);
    if (!ok) {
        play_state_ = NxpPlay_Stopped;
        set_error("Can't play file: " + err);
        return;
    }
    item_ = PlexItem();
    duration_ms_ = player->duration_ms() > 0 ? (uint32_t)player->duration_ms() : 0;
    title_ = path.substr(path.rfind('/') + 1);
    subtitle_ = "Local file";
    file_ = std::move(f);
    player_ = std::move(player);
    is_file_ = true;
    stream_offset_ms_ = 0;
    finished_flag_ = false;
    play_state_ = NxpPlay_Playing;
}

void App::job_seek() {
    int64_t target;
    bool file, was_paused;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        target = pending_seek_ms_;
        if (target < 0) return;
    }
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        if (!player_) {
            std::lock_guard<std::mutex> lk2(state_mu_);
            pending_seek_ms_ = -1;
            return;
        }
        file = is_file_;
        was_paused = player_->paused();
        if (file) player_->seek(target);
    }
    if (!file) {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            play_state_ = NxpPlay_Loading;
        }
        teardown_player();
        std::string err;
        bool ok = open_stream((uint32_t)target, err);
        if (ok && was_paused) {
            std::lock_guard<std::mutex> lk(play_mu_);
            if (player_) player_->set_paused(true);
        }
        std::lock_guard<std::mutex> lk(state_mu_);
        if (!ok) {
            play_state_ = NxpPlay_Stopped;
            set_error("Seek failed: " + err);
        } else {
            play_state_ = was_paused ? NxpPlay_Paused : NxpPlay_Playing;
        }
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    // A newer seek may have arrived meanwhile; it re-queued itself.
    if (pending_seek_ms_ == target) pending_seek_ms_ = -1;
    next_timeline_ms_ = 0;
}

void App::job_restart() {
    uint32_t pos;
    bool was_paused = false;
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        pos = position_locked();
        if (player_) was_paused = player_->paused();
    }
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        play_state_ = NxpPlay_Loading;
    }
    teardown_player();
    std::string err;
    bool ok = open_stream(pos, err);
    if (ok && was_paused) {
        std::lock_guard<std::mutex> lk(play_mu_);
        if (player_) player_->set_paused(true);
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    if (!ok) {
        play_state_ = NxpPlay_Stopped;
        set_error("Reconnect failed: " + err);
    } else {
        play_state_ = was_paused ? NxpPlay_Paused : NxpPlay_Playing;
    }
}

void App::job_stop(bool report, const char* reason) {
    if (report) report_timeline("stopped");
    teardown_player();
    {
        std::lock_guard<std::mutex> lk(play_mu_);
        stream_offset_ms_ = 0;
        is_file_ = false;
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    if (play_state_ != NxpPlay_Stopped) logf("app: playback %s", reason);
    play_state_ = NxpPlay_Stopped;
    pending_seek_ms_ = -1;
    title_.clear();
    subtitle_.clear();
    duration_ms_ = 0;
    item_ = PlexItem();
}

}  // namespace nxp
