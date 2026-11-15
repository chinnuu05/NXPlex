// NXPlex Tesla overlay: the remote control and library browser for the
// NXPlex background sysmodule, which does the actual playback (so video keeps
// going after this overlay closes). Talks to it over the "nxplex" named port.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxplex_ipc.h"

// ------------------------------------------------------------------ IPC client

namespace remote {

static Service g_srv;
static bool g_connected = false;

static void disconnect() {
    if (g_connected) serviceClose(&g_srv);
    g_connected = false;
}

static bool connect() {
    if (g_connected) return true;
    Handle h;
    if (R_FAILED(svcConnectToNamedPort(&h, NXP_PORT_NAME))) return false;
    serviceCreate(&g_srv, h);
    g_connected = true;
    return true;
}

// Every call drops the connection on failure so the next poll reconnects
// (e.g. after the sysmodule restarted).
static Result check(Result rc) {
    if (R_FAILED(rc)) disconnect();
    return rc;
}

static Result cmd(u32 c) { return g_connected ? check(serviceDispatch(&g_srv, c)) : -1; }

template <typename T>
static Result cmd_in(u32 c, const T& v) {
    return g_connected ? check(serviceDispatchIn(&g_srv, c, v)) : -1;
}

static Result get(u32 c, void* out, size_t size) {
    if (!g_connected) return -1;
    return check(serviceDispatch(&g_srv, c, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                 .buffers = {{out, size}}));
}

static Result put(u32 c, const void* in, size_t size) {
    if (!g_connected) return -1;
    return check(serviceDispatch(&g_srv, c, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                                 .buffers = {{in, size}}));
}

static u32 browse(const std::string& path, u32 start) {
    NxpBrowseArgs a = {};
    strncpy(a.path, path.c_str(), sizeof(a.path) - 1);
    a.start = start;
    u32 seq = 0;
    if (g_connected)
        check(serviceDispatchOut(&g_srv, NxpCmd_Browse, seq,
                                 .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                                 .buffers = {{&a, sizeof(a)}}));
    return seq;
}

// Tells the service whether Tesla is showing us, so the video window can
// step aside (the display shows either the video layer or Tesla's, not both).
// Failures are ignored on purpose: an older service doesn't know the command.
static int g_ui_state = NxpUi_Hidden;
static void send_ui_state(int state) {
    g_ui_state = state;
    u32 v = (u32)state;
    if (g_connected) serviceDispatchIn(&g_srv, NxpCmd_UiState, v);
}

static void play(const std::string& key, bool from_start) {
    NxpPlayArgs a = {};
    strncpy(a.key, key.c_str(), sizeof(a.key) - 1);
    a.from_start = from_start;
    put(NxpCmd_Play, &a, sizeof(a));
}

// ---- video preview (the PiP window is hidden while Tesla is open)

static std::vector<u8> g_frame_buf;
static std::vector<u16> g_frame;  // RGB565
static u32 g_frame_seq = 0;
static u16 g_frame_w = 0, g_frame_h = 0;

// Returns true when a new frame arrived. Failures don't drop the connection
// (an older service doesn't know this command).
static bool fetch_frame() {
    if (!g_connected) return false;
    if (g_frame_buf.empty()) g_frame_buf.resize(sizeof(NxpFrameHeader) + NXP_FRAME_MAX_W * NXP_FRAME_MAX_H * 2);
    u32 have = g_frame_seq;
    Result rc = serviceDispatchIn(&g_srv, NxpCmd_GetFrame, have,
                                  .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                  .buffers = {{g_frame_buf.data(), g_frame_buf.size()}});
    if (R_FAILED(rc)) return false;
    NxpFrameHeader hdr;
    memcpy(&hdr, g_frame_buf.data(), sizeof(hdr));
    if (hdr.seq == 0) {  // nothing to show (yet)
        g_frame_seq = 0;
        g_frame.clear();
        return false;
    }
    if (!hdr.has_pixels) return false;
    size_t n = (size_t)hdr.width * hdr.height;
    g_frame.resize(n);
    memcpy(g_frame.data(), g_frame_buf.data() + sizeof(hdr), n * 2);
    g_frame_w = hdr.width;
    g_frame_h = hdr.height;
    g_frame_seq = hdr.seq;
    return true;
}

// ---- sysmodule lifecycle

static bool sysmodule_running() {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, NXP_SYSMODULE_TID)) && pid != 0;
}

static bool sysmodule_installed() {
    bool ok = false;
    tsl::hlp::doWithSDCardHandle([&] {
        struct stat st;
        ok = stat("sdmc:/atmosphere/contents/4200000000504C58/exefs.nsp", &st) == 0;
    });
    return ok;
}

static Result sysmodule_launch() {
    NcmProgramLocation loc = {};
    loc.program_id = NXP_SYSMODULE_TID;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    return pmshellLaunchProgram(0, &loc, &pid);
}

// ---- shared state, polled by every screen

static NxpStatus g_status = {};
static bool g_have_status = false;
static std::string g_problem;  // connection problem shown instead of the UI
static u64 g_next_poll = 0, g_connect_deadline = 0;
static bool g_launched = false;

static u64 now_ms() { return armTicksToNs(armGetSystemTick()) / 1000000; }

// Call every frame; refreshes g_status 4x per second.
static void poll() {
    u64 now = now_ms();
    if (now < g_next_poll) return;
    g_next_poll = now + 250;
    if (!g_connected) {
        if (connect()) send_ui_state(g_ui_state);  // catch the service up after (re)connecting
    }
    if (!g_connected) {
        g_have_status = false;
        if (!g_launched) {
            if (!sysmodule_installed()) {
                g_problem = "NXPlex service missing: copy atmosphere/contents/4200000000504C58 to the SD card";
                return;
            }
            if (!sysmodule_running()) {
                Result rc = sysmodule_launch();
                if (R_FAILED(rc)) {
                    char b[96];
                    snprintf(b, sizeof(b), "Could not start the NXPlex service (0x%x)", rc);
                    g_problem = b;
                    return;
                }
            }
            g_launched = true;
            g_connect_deadline = now + 8000;
        }
        if (now > g_connect_deadline) {
            g_problem = "NXPlex service is not responding. See sd:/switch/nxplex/nxplex.log";
            if (now > g_connect_deadline + 5000) g_launched = false;  // try launching again
        } else {
            g_problem = "Starting NXPlex service...";
        }
        return;
    }
    if (R_SUCCEEDED(get(NxpCmd_GetStatus, &g_status, sizeof(g_status)))) {
        g_have_status = true;
        g_problem = g_status.api_version == NXP_API_VERSION ? "" : "Overlay and service versions differ: update both";
    } else {
        g_have_status = false;
    }
}

}  // namespace remote

// ------------------------------------------------------------------ helpers

static std::string fmt_time(u32 ms) {
    char b[24];
    u32 s = ms / 1000;
    if (s >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
    return b;
}

static const char* kCorners[4] = {"Top right", "Top left", "Bottom left", "Bottom right"};
static const char* kSizes[5] = {"Small", "Medium", "Large", "Extra large", "Huge"};
static const char* kQualities[5] = {"144p", "216p", "288p", "360p", "480p"};

// Set by a browse screen after starting playback; every browse screen pops
// itself on its next update until we're back at the main screen.
static bool g_pop_to_main = false;
static int g_browse_depth = 0;

static bool clicked(u64 keys) { return keys & HidNpadButton_A; }

// ------------------------------------------------------------------ preview + slider

// A TrackBar with the video preview drawn above the bar, as one element: when
// the slider is selected, Tesla scrolls to it and the picture stays in view.
class PreviewSeekBar : public tsl::elm::TrackBar {
public:
    using PreviewFn = std::function<void(tsl::gfx::Renderer*, s32 x, s32 y, s32 w, s32 h)>;
    PreviewSeekBar(const char* icon, u16 preview_h, PreviewFn preview)
        : TrackBar(icon), preview_h_(preview_h), preview_(std::move(preview)) {}

    void layout(u16, u16, u16, u16) override {
        setBoundaries(getX(), getY(), getWidth(), preview_h_ + tsl::style::TrackBarDefaultHeight);
    }
    void draw(tsl::gfx::Renderer* r) override {
        preview_(r, getX(), getY(), getWidth(), preview_h_);
        with_bar_bounds([&] { TrackBar::draw(r); });
    }
    void drawHighlight(tsl::gfx::Renderer* r) override {
        with_bar_bounds([&] { TrackBar::drawHighlight(r); });
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32 px, s32 py, s32 ix, s32 iy) override {
        bool handled = false;
        with_bar_bounds([&] { handled = TrackBar::onTouch(e, cx, cy, px, py, ix, iy); });
        return handled;
    }

private:
    // TrackBar draws relative to its own bounds: point them at the bar part.
    template <typename F>
    void with_bar_bounds(F f) {
        s32 x = getX(), y = getY(), w = getWidth(), h = getHeight();
        setBoundaries(x, y + preview_h_, w, tsl::style::TrackBarDefaultHeight);
        f();
        setBoundaries(x, y, w, h);
    }
    u16 preview_h_;
    PreviewFn preview_;
};

// ------------------------------------------------------------------ servers screen

class GuiServers : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("NXPlex", "Choose a Plex server");
        list_ = new tsl::elm::List();
        status_ = new tsl::elm::ListItem("Looking for servers...");
        list_->addItem(status_);
        frame->setContent(list_);
        remote::cmd(NxpCmd_RefreshServers);
        return frame;
    }

    void update() override {
        remote::poll();
        if (!remote::g_have_status) return;
        const NxpStatus& st = remote::g_status;
        if (waiting_select_) {
            if (st.servers_state == NxpServers_Ready && st.has_server) {
                tsl::goBack();
                return;
            }
            if (st.servers_state == NxpServers_Error) {
                waiting_select_ = false;
                status_->setText(st.error[0] ? st.error : "Could not connect");
            }
            return;
        }
        if (st.servers_state == NxpServers_Error && !shown_) {
            status_->setText(st.error[0] ? st.error : "Could not list servers");
            return;
        }
        if (st.servers_state != NxpServers_Ready || shown_) return;
        NxpServers sv;
        if (R_FAILED(remote::get(NxpCmd_GetServers, &sv, sizeof(sv)))) return;
        shown_ = true;
        removeFocus();
        list_->clear();
        status_ = new tsl::elm::ListItem(sv.count ? "Pick the server to watch from" : "No servers found on this account");
        list_->addItem(new tsl::elm::CategoryHeader("Servers"));
        for (u32 i = 0; i < sv.count; i++) {
            auto item = new tsl::elm::ListItem(sv.servers[i].name,
                                               sv.servers[i].selected ? "in use" : (sv.servers[i].online ? "" : "offline"));
            item->setClickListener([this, i, item](u64 k) {
                if (!clicked(k)) return false;
                item->setValue("connecting...");
                remote::cmd_in<u32>(NxpCmd_SelectServer, i);
                waiting_select_ = true;
                return true;
            });
            list_->addItem(item);
        }
        list_->addItem(status_);
        restoreFocus();
    }

private:
    tsl::elm::List* list_ = nullptr;
    tsl::elm::ListItem* status_ = nullptr;
    bool shown_ = false, waiting_select_ = false;
};

// ------------------------------------------------------------------ browse screen

class GuiBrowse : public tsl::Gui {
public:
    GuiBrowse(const std::string& path, const std::string& title) : path_(path), title_(title) { g_browse_depth++; }
    ~GuiBrowse() { g_browse_depth--; }

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("NXPlex", title_);
        list_ = new tsl::elm::List();
        // NB: never remove items from a tsl List: libtesla's removeItem()
        // underflows its focus index and crashes on the next scroll. The
        // status line is a header we only re-label, and pages are appended.
        status_ = new tsl::elm::CategoryHeader("Loading...");
        list_->addItem(status_);
        frame->setContent(list_);
        seq_ = remote::browse(path_, 0);
        return frame;
    }

    void update() override {
        if (g_pop_to_main) {
            if (g_browse_depth <= 1) g_pop_to_main = false;
            tsl::goBack();
            return;
        }
        remote::poll();
        if (done_ || !remote::g_have_status) return;
        if (!seq_) {
            seq_ = remote::browse(path_, loaded_);
            return;
        }
        const NxpStatus& st = remote::g_status;
        if (st.browse_seq != seq_) {
            // Another screen browsed meanwhile: ask again once it settles.
            if (st.browse_state != NxpBrowse_Loading) seq_ = remote::browse(path_, loaded_);
            return;
        }
        if (st.browse_state == NxpBrowse_Loading) return;
        NxpBrowse* b = new NxpBrowse();
        if (R_SUCCEEDED(remote::get(NxpCmd_GetBrowse, b, sizeof(*b))) && b->seq == seq_) show_page(*b);
        delete b;
    }

private:
    static const u32 kMaxItems = 1000;

    void show_page(const NxpBrowse& b) {
        if (b.state == NxpBrowse_Error) {
            status_->setText(std::string("Error: ") + b.error);
            done_ = true;
            return;
        }
        bool first = loaded_ == 0;
        for (u32 i = 0; i < b.count; i++) add_item(b.items[i]);
        loaded_ = b.offset + b.count;
        u32 total = b.total > loaded_ ? b.total : loaded_;
        char t[64];
        if (loaded_ == 0) snprintf(t, sizeof(t), "Nothing here");
        else if (loaded_ < total && loaded_ < kMaxItems && b.count > 0) snprintf(t, sizeof(t), "Loading... %u of %u", loaded_, total);
        else snprintf(t, sizeof(t), "%u item%s", loaded_, loaded_ == 1 ? "" : "s");
        status_->setText(t);
        if (first) restoreFocus();
        // Fetch the next page right away (items are appended, focus is kept).
        if (loaded_ < total && loaded_ < kMaxItems && b.count > 0) seq_ = remote::browse(path_, loaded_);
        else done_ = true;
    }

    void add_item(const NxpItem& it) {
        std::string type = it.type, text, value;
        char b[200];
        if (type == "episode") {
            if (path_ == "/library/onDeck")
                snprintf(b, sizeof(b), "%s S%dE%d", it.grandparent, it.parent_index, it.index);
            else if (it.index >= 0)
                snprintf(b, sizeof(b), "%d. %s", it.index, it.title);
            else
                snprintf(b, sizeof(b), "%s", it.title);
            text = b;
        } else if (type == "movie" && it.year) {
            snprintf(b, sizeof(b), "%s (%u)", it.title, it.year);
            text = b;
        } else {
            text = it.title;
        }
        if (it.is_leaf) {
            if (it.view_offset_ms) value = "resume " + fmt_time(it.view_offset_ms);
            else value = fmt_time(it.duration_ms);
        } else if (it.leaf_count) {
            u32 left = it.leaf_count > it.viewed_leaf_count ? it.leaf_count - it.viewed_leaf_count : 0;
            value = left ? std::to_string(left) + " unwatched" : "watched";
        }
        auto item = new tsl::elm::ListItem(text, value);
        std::string key = it.key, title = it.title;
        bool leaf = it.is_leaf;
        item->setClickListener([key, title, leaf, item](u64 k) {
            if (leaf && (k & (HidNpadButton_A | HidNpadButton_Y))) {
                remote::play(key, (k & HidNpadButton_Y) != 0);  // Y = from the beginning
                item->setValue("starting...");
                g_pop_to_main = true;
                return true;
            }
            if (!leaf && clicked(k)) {
                tsl::changeTo<GuiBrowse>(key, title);
                return true;
            }
            return false;
        });
        list_->addItem(item);
    }

    std::string path_, title_;
    tsl::elm::List* list_ = nullptr;
    tsl::elm::CategoryHeader* status_ = nullptr;
    u32 seq_ = 0, loaded_ = 0;
    bool done_ = false;
};

// ------------------------------------------------------------------ main screen

class GuiMain : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("NXPlex", "Plex while you play");
        list_ = new tsl::elm::List();
        frame->setContent(list_);
        build(Shape::Connecting);
        return frame;
    }

    void update() override {
        remote::poll();
        Shape shape = current_shape();
        bool episode = remote::g_have_status && remote::g_status.is_episode;
        if (shape != shape_ || (shape == Shape::Playing && episode != built_episode_)) build(shape);
        refresh();
        if (shape_ == Shape::Playing && remote::now_ms() >= next_frame_at_) {
            next_frame_at_ = remote::now_ms() + 33;  // up to ~30 fps; the video is 24
            remote::fetch_frame();
        }
        if (pending_seek_pct_ >= 0 && remote::now_ms() >= seek_send_at_) {
            u32 dur = remote::g_status.duration_ms;
            if (dur) remote::cmd_in<u32>(NxpCmd_SeekAbsolute, (u32)((u64)dur * (u32)pending_seek_pct_ / 100));
            pending_seek_pct_ = -1;
            seek_hold_until_ = remote::now_ms() + 3000;  // let the restart settle before following status again
        }
        if (pending_volume_ >= 0 && remote::now_ms() >= volume_send_at_) {
            remote::cmd_in<u32>(NxpCmd_SetVolume, (u32)pending_volume_);
            pending_volume_ = -1;
        }
    }

private:
    enum class Shape { Connecting, SignedOut, PinPending, NoServer, Idle, Playing };

    Shape current_shape() {
        if (!remote::g_have_status || !remote::g_problem.empty()) return Shape::Connecting;
        const NxpStatus& st = remote::g_status;
        if (st.auth_state == NxpAuth_SignedOut) return Shape::SignedOut;
        if (st.auth_state == NxpAuth_PinPending) return Shape::PinPending;
        if (!st.has_server) return Shape::NoServer;
        return st.play_state == NxpPlay_Stopped ? Shape::Idle : Shape::Playing;
    }

    tsl::elm::ListItem* add(const std::string& text, const std::string& value, std::function<void()> on_a) {
        auto item = new tsl::elm::ListItem(text, value);
        item->setClickListener([on_a](u64 k) {
            if (!clicked(k) || !on_a) return false;
            on_a();
            return true;
        });
        list_->addItem(item);
        return item;
    }

    void build(Shape shape) {
        shape_ = shape;
        built_episode_ = remote::g_have_status && remote::g_status.is_episode;
        removeFocus();
        list_->clear();
        status_ = title_ = sub_ = pause_ = corner_ = size_ = quality_ = account_ = server_ = autoplay_ = nullptr;
        volume_ = seek_ = nullptr;
        pos_header_ = nullptr;
        pending_seek_pct_ = -1;
        pos_text_.clear();
        signout_armed_ = false;
        status_text_.clear();
        last_title_.clear();
        pause_text_.clear();
        const NxpStatus& st = remote::g_status;

        status_ = add("Connecting...", "", nullptr);  // status / error line

        switch (shape) {
        case Shape::Connecting:
            break;
        case Shape::SignedOut:
            list_->addItem(new tsl::elm::CategoryHeader("Account"));
            add("Sign in to Plex", "", [] { remote::cmd(NxpCmd_StartLogin); });
            break;
        case Shape::PinPending:
            list_->addItem(new tsl::elm::CategoryHeader("Sign in"));
            list_->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
                               r->drawString("On your phone or computer, open", false, x + 20, y + 30, 20,
                                             r->a(tsl::style::color::ColorDescription));
                               r->drawString("plex.tv/link", false, x + 20, y + 70, 34, r->a(tsl::style::color::ColorText));
                               r->drawString("and enter this code:", false, x + 20, y + 110, 20,
                                             r->a(tsl::style::color::ColorDescription));
                               const char* code = remote::g_status.pin_code[0] ? remote::g_status.pin_code : "....";
                               r->drawString(code, true, x + 20, y + 180, 56, r->a(tsl::style::color::ColorHighlight));
                           }),
                           210);
            add("Cancel", "", [] { remote::cmd(NxpCmd_Logout); });
            break;
        case Shape::NoServer:
            list_->addItem(new tsl::elm::CategoryHeader("Server"));
            add("Choose your Plex server", "", [] { tsl::changeTo<GuiServers>(); });
            break;
        case Shape::Playing:
            // The header shows the time; right under it the live preview and
            // the position slider, as one element (hold Left/Right or drag;
            // the seek is sent once it stops moving).
            pos_header_ = new tsl::elm::CategoryHeader("Now playing");
            list_->addItem(pos_header_);
            seek_ = new PreviewSeekBar("\u25B6", kPreviewHeight,
                                       [this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
                                           draw_preview(r, x, y, w, h);
                                       });
            seek_->setProgress(st.duration_ms ? (u8)((u64)st.position_ms * 100 / st.duration_ms) : 0);
            seek_->setValueChangedListener([this](u8 v) {
                pending_seek_pct_ = v;
                seek_send_at_ = remote::now_ms() + 500;
            });
            list_->addItem(seek_);
            title_ = add(st.title, "", nullptr);
            sub_ = add(st.subtitle, "", nullptr);
            pause_ = add("Pause", "", [] { remote::cmd_in<u32>(NxpCmd_SetPaused, 2); });
            add("Back 10 seconds", "", [] { remote::cmd_in<s32>(NxpCmd_SeekRelative, -10000); });
            add("Forward 30 seconds", "", [] { remote::cmd_in<s32>(NxpCmd_SeekRelative, 30000); });
            if (built_episode_) add("Next episode", "", [] { remote::cmd(NxpCmd_NextEpisode); });
            volume_ = new tsl::elm::TrackBar("");
            volume_->setProgress(st.volume);
            volume_->setValueChangedListener([this](u8 v) {
                pending_volume_ = v;
                volume_send_at_ = remote::now_ms() + 120;  // send once the stick settles
            });
            list_->addItem(volume_);
            corner_ = add("Window position", "", [] {
                const NxpStatus& s = remote::g_status;
                remote::cmd_in<u32>(NxpCmd_SetLayout, (u32)(((s.corner + 1) % 4) | (s.size << 8)));
            });
            size_ = add("Window size", "", [] {
                const NxpStatus& s = remote::g_status;
                remote::cmd_in<u32>(NxpCmd_SetLayout, (u32)(s.corner | (((s.size + 1) % 5) << 8)));
            });
            add("Stop", "", [] { remote::cmd(NxpCmd_Stop); });
            [[fallthrough]];
        case Shape::Idle:
            list_->addItem(new tsl::elm::CategoryHeader("Watch"));
            add("Continue Watching", "", [] { tsl::changeTo<GuiBrowse>("/library/onDeck", "Continue Watching"); });
            add("Libraries", "", [] { tsl::changeTo<GuiBrowse>("/library/sections", "Libraries"); });
            list_->addItem(new tsl::elm::CategoryHeader("Settings"));
            quality_ = add("Video quality", "", [] {
                remote::cmd_in<u32>(NxpCmd_SetQuality, (u32)((remote::g_status.quality + 1) % 5));
            });
            autoplay_ = add("Autoplay next episode", "", [] {
                remote::cmd_in<u32>(NxpCmd_SetAutoplay, remote::g_status.autoplay ? 0u : 1u);
            });
            server_ = add("Server", "", [] { tsl::changeTo<GuiServers>(); });
            account_ = add("Account", "", [this] {
                if (!signout_armed_) {
                    signout_armed_ = true;
                    account_->setValue("A again = sign out");
                } else {
                    remote::cmd(NxpCmd_Logout);
                }
            });
            add("Stop NXPlex service", "frees memory", [] { remote::cmd(NxpCmd_Quit); });
            break;
        }
        restoreFocus();
    }

    static const u16 kPreviewHeight = 215;

    // Live video inside the menu: scaled (nearest) and converted to Tesla's
    // RGBA4444 once per new frame, then blitted every frame.
    void draw_preview(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
        s32 bh = h - 10, bw = bh * 16 / 9;
        if (bw > w - 10) {
            bw = w - 10;
            bh = bw * 9 / 16;
        }
        s32 bx = x + (w - bw) / 2, by = y + 5;
        r->drawRect(bx, by, bw, bh, tsl::Color{0x0, 0x0, 0x0, 0xF});
        const auto& f = remote::g_frame;
        if (f.empty() || !remote::g_frame_w || !remote::g_frame_h) {
            r->drawString("Waiting for video...", false, bx + 20, by + bh / 2 + 8, 20,
                          r->a(tsl::style::color::ColorDescription));
            return;
        }
        s32 fw = remote::g_frame_w, fh = remote::g_frame_h, dw = bw, dh = bh;
        if (fw * bh > fh * bw) dh = bw * fh / fw;  // letterbox
        else dw = bh * fw / fh;                    // pillarbox (e.g. 4:3)
        if (cache_seq_ != remote::g_frame_seq || cache_w_ != dw || cache_h_ != dh) {
            cache_.resize((size_t)dw * dh);
            for (s32 j = 0; j < dh; j++) {
                const u16* src = f.data() + (size_t)(j * fh / dh) * fw;
                u16* dst = cache_.data() + (size_t)j * dw;
                for (s32 i = 0; i < dw; i++) {
                    u16 p = src[i * fw / dw];  // RGB565 -> Tesla RGBA4444 (r low nibble, a high)
                    dst[i] = (u16)((p >> 12) | (((p >> 7) & 0xF) << 4) | (((p >> 1) & 0xF) << 8) | 0xF000);
                }
            }
            cache_seq_ = remote::g_frame_seq;
            cache_w_ = dw;
            cache_h_ = dh;
        }
        s32 ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
        for (s32 j = 0; j < dh; j++) {
            const u16* row = cache_.data() + (size_t)j * dw;
            for (s32 i = 0; i < dw; i++) r->setPixel(ox + i, oy + j, tsl::Color(row[i]));
        }
    }

    // Updates texts in place (no rebuild).
    void refresh() {
        const NxpStatus& st = remote::g_status;
        if (status_) {
            std::string s;
            if (!remote::g_problem.empty()) s = remote::g_problem;
            else if (!remote::g_have_status) s = "Connecting...";
            else if (st.error[0]) s = st.error;
            else if (shape_ == Shape::PinPending) s = st.pin_code[0] ? "Waiting for the code to be entered" : "Getting a code...";
            else if (shape_ == Shape::SignedOut) s = "Not signed in";
            else if (shape_ == Shape::NoServer) s = std::string("Signed in as ") + st.account;
            else if (st.play_state == NxpPlay_Loading) s = "Loading...";
            else s = st.server_name[0] ? std::string("Connected to ") + st.server_name : "Ready";
            if (s != status_text_) {
                status_text_ = s;
                status_->setText(s);
            }
        }
        if (!remote::g_have_status) return;
        if (title_ && st.title != last_title_) {
            last_title_ = st.title;
            title_->setText(st.title);
            sub_->setText(st.subtitle);
        }
        if (pos_header_) {
            std::string v;
            if (pending_seek_pct_ >= 0)
                v = "Jump to " + fmt_time((u32)((u64)st.duration_ms * (u32)pending_seek_pct_ / 100)) + " / " +
                    fmt_time(st.duration_ms);
            else
                v = std::string(st.play_state == NxpPlay_Loading ? "Loading  " : "Now playing  ") +
                    fmt_time(st.position_ms) + " / " + fmt_time(st.duration_ms);
            if (v != pos_text_) {
                pos_text_ = v;
                pos_header_->setText(v);
            }
        }
        if (seek_ && pending_seek_pct_ < 0 && remote::now_ms() >= seek_hold_until_ && st.duration_ms)
            seek_->setProgress((u8)((u64)st.position_ms * 100 / st.duration_ms));
        if (pause_) {
            const char* t = st.play_state == NxpPlay_Paused ? "Resume" : "Pause";
            if (pause_text_ != t) {
                pause_text_ = t;
                pause_->setText(t);
            }
        }
        if (volume_ && pending_volume_ < 0 && getFocusedElement() != volume_) volume_->setProgress(st.volume);
        if (corner_) corner_->setValue(kCorners[st.corner & 3]);
        if (size_) size_->setValue(kSizes[st.size % 5]);
        if (quality_) quality_->setValue(kQualities[st.quality % 5]);
        if (autoplay_) autoplay_->setValue(st.autoplay ? "On" : "Off");
        if (server_) server_->setValue(st.server_name);
        if (account_ && !signout_armed_) account_->setValue(st.account);
    }

    tsl::elm::List* list_ = nullptr;
    Shape shape_ = Shape::Connecting;
    tsl::elm::ListItem *status_ = nullptr, *title_ = nullptr, *sub_ = nullptr, *pause_ = nullptr;
    tsl::elm::ListItem *corner_ = nullptr, *size_ = nullptr, *quality_ = nullptr, *account_ = nullptr,
                       *server_ = nullptr, *autoplay_ = nullptr;
    bool built_episode_ = false;
    tsl::elm::TrackBar* volume_ = nullptr;
    tsl::elm::TrackBar* seek_ = nullptr;
    std::vector<u16> cache_;  // preview in Tesla's RGBA4444
    u32 cache_seq_ = 0;
    s32 cache_w_ = 0, cache_h_ = 0;
    u64 next_frame_at_ = 0;
    tsl::elm::CategoryHeader* pos_header_ = nullptr;
    int pending_seek_pct_ = -1;
    u64 seek_send_at_ = 0, seek_hold_until_ = 0;
    std::string pos_text_;
    std::string status_text_, last_title_, pause_text_;
    int pending_volume_ = -1;
    u64 volume_send_at_ = 0;
    bool signout_armed_ = false;
};

// ------------------------------------------------------------------ overlay

class NxplexOverlay : public tsl::Overlay {
public:
    void initServices() override { pmshellInitialize(); }
    void exitServices() override {
        // B on our main screen closes this overlay: return to the Tesla menu
        // *visibly* (like other overlays do), which is also what the service
        // assumes when we report NxpUi_Exiting.
        tsl::setNextOverlay("sdmc:/switch/.overlays/ovlmenu.ovl", "");
        remote::send_ui_state(NxpUi_Exiting);
        remote::disconnect();
        pmshellExit();
    }
    void onShow() override { remote::send_ui_state(NxpUi_Shown); }
    void onHide() override { remote::send_ui_state(NxpUi_Hidden); }
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<GuiMain>(); }
};

int main(int argc, char** argv) { return tsl::loop<NxplexOverlay>(argc, argv); }
