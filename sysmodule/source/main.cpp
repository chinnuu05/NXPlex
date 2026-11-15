// NXPlex background sysmodule (title 4200000000504C58).
//
// Started on demand by the NXPlex Tesla overlay; keeps playing while Tesla
// or other overlays are open. Serves the overlay over the "nxplex" named
// port (see common/nxplex_ipc.h).
//
// Shortcuts while a video plays (hold L + R3, then press):
//   Minus stop, D-pad Up pause/resume, D-pad Left/Right -10 s/+30 s,
//   ZR/R volume, ZL move window.
// Only the D-pad counts (not stick directions): pressing R3 wobbles the right
// stick, which used to fire stray "Up/Left/Right" presses.
// (D-pad Down is left alone: L + Down + R3 opens Tesla.)
#include <switch.h>

#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "app.hpp"
#include "config.hpp"
#include "ipc_server.hpp"
#include "net.hpp"
#include "nxplex_ipc.h"
#include "pip.hpp"
#include "platform.hpp"

using namespace nxp;

#ifndef NXP_SYS_VERSION  // set from ../VERSION by the Makefile
#define NXP_SYS_VERSION "dev"
#endif

// ------------------------------------------------------------------ runtime

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_nv_transfermem_size = 0x40000;
ViLayerFlags __nx_vi_stray_layer_flags = (ViLayerFlags)0;
void __libnx_init_time(void);

#define HEAP_SIZE (6 * 1024 * 1024)
void __libnx_initheap(void) {
    static char inner_heap[HEAP_SIZE] alignas(0x1000);
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

static const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size = 0x4000,
    .tcp_rx_buf_size = 0x10000,
    .tcp_tx_buf_max_size = 0x8000,
    .tcp_rx_buf_max_size = 0x20000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 2,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_Auto,
};

void __appInit(void) {
    if (R_FAILED(smInitialize())) fatalThrow(MAKERESULT(Module_HomebrewLoader, 1));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(fsInitialize())) fatalThrow(MAKERESULT(Module_HomebrewLoader, 2));
    fsdevMountSdmc();
    if (R_SUCCEEDED(timeInitialize())) __libnx_init_time();  // TLS certificate dates
    csrngInitialize();                                        // mbedTLS entropy
    hidInitialize();
    socketInitialize(&kSocketConfig);
}

void __appExit(void) {
    socketExit();
    hidExit();
    csrngExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}
}

// ------------------------------------------------------------------ logging

static FILE* g_log = nullptr;
static std::mutex g_log_mu;
static void log_to_file(const char* line) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (!g_log) return;
    fprintf(g_log, "[%9.3f] %s\n", now_ms() / 1000.0, line);
    fflush(g_log);
}

// ------------------------------------------------------------------ globals

static PipDisplay g_display;
static SwitchAudio g_audio;  // large aligned buffers: static, not heap/stack
static App* g_app = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_last_activity{0};

// ------------------------------------------------------------------ Tesla watcher

// The display can't show our video layer and Tesla's menu at the same time
// (Tesla opens but stays invisible), so the video steps aside while Tesla is
// open. Tesla runs in another process, so its state is inferred from:
//   - its open/close combo (read from sd:/config/tesla/config.ini),
//   - B on the Tesla menu (closes it) and A (opens another overlay),
//   - the NXPlex overlay reporting when Tesla shows/hides/closes it.
static std::atomic<int> g_ui_event{-1};

class TeslaWatcher {
public:
    enum State { Closed, Menu, Nxplex, Other };

    void load_combo() {
        combo_ = HidNpadButton_L | HidNpadButton_Down | HidNpadButton_StickR;
        FILE* f = fopen("sdmc:/config/tesla/config.ini", "r");
        if (!f) return;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* v = strstr(line, "key_combo");
            if (!v || !(v = strchr(v, '='))) continue;
            u64 keys = 0;
            for (char* tok = strtok(v + 1, "+ \t\r\n"); tok; tok = strtok(nullptr, "+ \t\r\n")) keys |= key(tok);
            if (keys) combo_ = keys;
        }
        fclose(f);
        logf("tesla: combo 0x%llx", (unsigned long long)combo_);
    }

    // Call every loop with this frame's pad state. Returns true while Tesla is (probably) open.
    bool update(u64 down, u64 held) {
        int ev = g_ui_event.exchange(-1);
        if (ev == NxpUi_Shown) set(Nxplex, "NXPlex overlay shown");
        else if (ev == NxpUi_Hidden) set(Closed, "NXPlex overlay hidden");
        else if (ev == NxpUi_Exiting) set(Menu, "NXPlex overlay closed");

        if ((held & combo_) == combo_ && (down & combo_)) {
            set(state_ == Closed ? Menu : Closed, "combo");
        } else if (state_ == Menu) {
            if (down & HidNpadButton_B) set(Closed, "B on the Tesla menu");
            else if (down & HidNpadButton_A) set(Other, "A on the Tesla menu");
        } else if (state_ == Other) {
            if (down & HidNpadButton_B) set(Menu, "B in another overlay");
        }
        return state_ != Closed;
    }

private:
    static u64 key(const char* n) {
        static const struct {
            const char* name;
            u64 key;
        } map[] = {{"L", HidNpadButton_L},       {"R", HidNpadButton_R},         {"ZL", HidNpadButton_ZL},
                   {"ZR", HidNpadButton_ZR},     {"SL", HidNpadButton_AnySL},    {"SR", HidNpadButton_AnySR},
                   {"DLEFT", HidNpadButton_Left}, {"DUP", HidNpadButton_Up},     {"DRIGHT", HidNpadButton_Right},
                   {"DDOWN", HidNpadButton_Down}, {"A", HidNpadButton_A},        {"B", HidNpadButton_B},
                   {"X", HidNpadButton_X},       {"Y", HidNpadButton_Y},         {"LS", HidNpadButton_StickL},
                   {"RS", HidNpadButton_StickR}, {"MINUS", HidNpadButton_Minus}, {"PLUS", HidNpadButton_Plus}};
        for (auto& m : map)
            if (strcasecmp(n, m.name) == 0) return m.key;
        return 0;
    }
    void set(State s, const char* why) {
        static const char* names[] = {"closed", "menu", "NXPlex", "other overlay"};
        if (s == state_) return;
        logf("tesla: %s -> %s (%s)", names[state_], names[s], why);
        state_ = s;
    }
    u64 combo_ = 0;
    State state_ = Closed;
};

// ------------------------------------------------------------------ display thread

static void display_thread(void*) {
    PadState pad;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt);
    padInitializeAny(&pad);
    const u64 kMod = HidNpadButton_L | HidNpadButton_StickR;
    const u64 kActions = HidNpadButton_Up | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_ZR |
                         HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_Minus;
    uint64_t idle_since = 0;
    uint64_t loops = 0, loops_since = now_ms();  // diagnostics: detect busy loops
    TeslaWatcher tesla;
    tesla.load_combo();

    while (g_running) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad), held = padGetButtons(&pad);
        g_display.set_hidden(tesla.update(down, held));

        bool want = g_app->playing_or_loading();
        if (want) {
            idle_since = 0;
            if (!g_display.is_open()) g_display.open();
        } else if (g_display.is_open()) {
            if (!idle_since) idle_since = now_ms();
            if (now_ms() - idle_since > 500) g_display.close();
        }

        bool active = false;
        int wait = g_app->video_tick(active);

        NxpStatus st;
        g_app->get_status(st);
        if (st.duration_ms) g_display.set_progress((float)st.position_ms / (float)st.duration_ms);
        g_display.set_paused(st.play_state == NxpPlay_Paused);
        g_display.tick_osd();
        loops++;
        if (now_ms() - loops_since >= 30000) {
            if (want) logf("display loop: %llu iterations in 30 s", (unsigned long long)loops);
            loops = 0;
            loops_since = now_ms();
        }

        // Controller shortcuts (only while something is playing).
        if (want && (held & kMod) == kMod && (down & kActions)) {
            g_last_activity = now_ms();
            if (down & HidNpadButton_Minus) {
                logf("shortcut: stop");
                g_app->cmd_stop();
            } else {
                if (down & HidNpadButton_Up) g_app->cmd_set_paused(2);
                if (down & HidNpadButton_Left) {
                    g_app->cmd_seek_relative(-10000);
                    g_display.flash_progress(2500);
                }
                if (down & HidNpadButton_Right) {
                    g_app->cmd_seek_relative(30000);
                    g_display.flash_progress(2500);
                }
                if (down & (HidNpadButton_ZR | HidNpadButton_R)) {
                    int v = st.volume + ((down & HidNpadButton_ZR) ? 10 : -10);
                    v = v < 0 ? 0 : (v > 100 ? 100 : v);
                    g_app->cmd_set_volume(v);
                    g_display.flash_volume(v, 1500);
                }
                if (down & HidNpadButton_ZL) g_app->cmd_set_layout((st.corner + 1) % 4, st.size);
            }
        }

        if (!active) wait = want ? 10 : 50;  // idle: poll slowly
        if (wait > 10) wait = 10;
        if (wait > 0) sleep_ms((uint32_t)wait);
    }
    g_display.close();
}

// ------------------------------------------------------------------ IPC

static std::string buf_string(const IpcRequest& r) {
    if (!r.in_buf || !r.in_buf_size) return "";
    return std::string((const char*)r.in_buf, strnlen((const char*)r.in_buf, r.in_buf_size));
}

template <typename T>
static bool in_val(const IpcRequest& r, T& v) {
    if (r.size < sizeof(T)) return false;
    memcpy(&v, r.data, sizeof(T));
    return true;
}

template <typename T>
static Result out_buf(const IpcRequest& r, const T& v) {
    if (!r.out_buf || r.out_buf_size < sizeof(T)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memcpy(r.out_buf, &v, sizeof(T));
    return 0;
}

static Result handle(const IpcRequest& r, u8* out, size_t& out_size) {
    g_last_activity = now_ms();
    const Result kBad = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    u32 u = 0;
    s32 s = 0;
    switch (r.cmd) {
    case NxpCmd_GetStatus: {
        static NxpStatus st;
        g_app->get_status(st);
        return out_buf(r, st);
    }
    case NxpCmd_StartLogin: g_app->cmd_start_login(); return 0;
    case NxpCmd_Logout: g_app->cmd_logout(); return 0;
    case NxpCmd_RefreshServers: g_app->cmd_refresh_servers(); return 0;
    case NxpCmd_GetServers: {
        static NxpServers sv;
        g_app->get_servers(sv);
        return out_buf(r, sv);
    }
    case NxpCmd_SelectServer:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_select_server(u);
        return 0;
    case NxpCmd_Browse: {
        if (!r.in_buf || r.in_buf_size < sizeof(NxpBrowseArgs)) return kBad;
        NxpBrowseArgs a;
        memcpy(&a, r.in_buf, sizeof(a));
        a.path[sizeof(a.path) - 1] = 0;
        u32 seq = g_app->cmd_browse(a.path, a.start);
        memcpy(out, &seq, 4);
        out_size = 4;
        return 0;
    }
    case NxpCmd_GetBrowse: {
        static NxpBrowse b;
        g_app->get_browse(b);
        return out_buf(r, b);
    }
    case NxpCmd_Play: {
        if (!r.in_buf || r.in_buf_size < sizeof(NxpPlayArgs)) return kBad;
        NxpPlayArgs a;
        memcpy(&a, r.in_buf, sizeof(a));
        a.key[sizeof(a.key) - 1] = 0;
        g_app->cmd_play(a.key, a.from_start != 0);
        return 0;
    }
    case NxpCmd_PlayFile: g_app->cmd_play_file(buf_string(r)); return 0;
    case NxpCmd_SetPaused:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_set_paused((int)u);
        return 0;
    case NxpCmd_Stop: g_app->cmd_stop(); return 0;
    case NxpCmd_SeekRelative:
        if (!in_val(r, s)) return kBad;
        g_app->cmd_seek_relative(s);
        g_display.flash_progress(2500);
        return 0;
    case NxpCmd_SeekAbsolute:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_seek_absolute(u);
        g_display.flash_progress(2500);
        return 0;
    case NxpCmd_SetVolume:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_set_volume((int)u);
        g_display.flash_volume((int)u, 1500);
        return 0;
    case NxpCmd_SetLayout:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_set_layout((int)(u & 0xFF), (int)((u >> 8) & 0xFF));
        return 0;
    case NxpCmd_SetQuality:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_set_quality((int)u);
        return 0;
    case NxpCmd_UiState:
        if (!in_val(r, u)) return kBad;
        g_ui_event = (int)u;
        return 0;
    case NxpCmd_NextEpisode: g_app->cmd_next_episode(); return 0;
    case NxpCmd_SetAutoplay:
        if (!in_val(r, u)) return kBad;
        g_app->cmd_set_autoplay(u != 0);
        return 0;
    case NxpCmd_Quit: g_app->cmd_quit(); return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

// ------------------------------------------------------------------ main

int main(int, char**) {
    mkdir("sdmc:/switch/nxplex", 0777);
    g_log = fopen("sdmc:/switch/nxplex/nxplex.log", "a");
    log_set_sink(log_to_file);
    logf("==== NXPlex sysmodule " NXP_SYS_VERSION " starting (HOS %u.%u.%u)", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
         HOSVER_MICRO(hosversionGet()));

    if (!net_init()) logf("warning: TLS init failed");
    g_audio.init();

    Config cfg;
    cfg.load(default_config_path());
    if (cfg.get("client_id").empty()) {
        cfg.set("client_id", new_client_id());
        cfg.save();
    }
    PlexClient client;
    client.client_id = cfg.get("client_id");
    client.platform = "Nintendo Switch";
    client.device = "Nintendo Switch";
    client.device_name = "NXPlex on Switch";
    client.version = NXP_SYS_VERSION;

    App::Hooks hooks;
    hooks.set_volume = [](int v) { g_audio.set_volume(v); };
    hooks.set_layout = [](int c, int s) { g_display.set_layout(c, s); };
    g_app = new App(cfg, client, g_audio, g_display, hooks);
    g_app->start();

    nxp::Thread disp;
    disp.start(display_thread, nullptr, 0x10000, 0x3A);  // below Tesla's UI threads

    IpcServer server;
    Result rc = server.init(NXP_PORT_NAME, 4);
    if (R_FAILED(rc)) logf("fatal: cannot register port: 0x%x", rc);
    g_last_activity = now_ms();
    while (R_SUCCEEDED(rc) && !g_app->quit_requested()) {
        if (!server.process(500'000'000ULL, handle)) break;
        // Leave memory free when nobody needs us: nothing playing, no
        // overlay connected, nothing asked for 10 minutes.
        if (!g_app->playing_or_loading() && server.session_count() == 0 && now_ms() - g_last_activity > 600000) {
            logf("idle for 10 minutes; exiting");
            break;
        }
    }
    logf("shutting down");
    server.exit();
    g_running = false;
    disp.join();
    g_app->stop();
    delete g_app;
    g_audio.exit();
    net_exit();
    logf("==== exit");
    if (g_log) fclose(g_log);
    return 0;
}
