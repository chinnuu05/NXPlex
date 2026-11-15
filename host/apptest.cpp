// apptest: end-to-end test of the App (the code the Switch sysmodule runs)
// against a real Plex server, using the sign-in saved by `nxplex-cli login`
// and `nxplex-cli use`. Plays an item, seeks, pauses, changes quality, stops,
// and checks that Plex recorded the resume position.
//
//   apptest [item-key]        default: first episode of "Continue Watching"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.hpp"
#include "platform.hpp"
#include "sim.hpp"

using namespace nxp;

static int g_fail = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        bool ok_ = (cond);                       \
        printf("%s  ", ok_ ? "PASS" : "FAIL");   \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
        if (!ok_) g_fail++;                      \
    } while (0)

static std::atomic<bool> g_run{true};
static App* g_app = nullptr;

static void display_loop(void*) {
    while (g_run) {
        bool active = false;
        int w = g_app->video_tick(active);
        sleep_ms(w > 0 ? (uint32_t)(w > 10 ? 10 : w) : 0);
    }
}

static NxpStatus status() {
    NxpStatus s;
    g_app->get_status(s);
    return s;
}

static bool wait_for(const char* what, uint32_t ms, bool (*pred)(const NxpStatus&)) {
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (pred(status())) return true;
        sleep_ms(100);
    }
    NxpStatus s = status();
    printf("      (timed out waiting for %s; play_state=%d error='%s')\n", what, s.play_state, s.error);
    return false;
}

static void log_stdout(const char* l) { printf("      [log] %s\n", l); }

int main(int argc, char** argv) {
    log_set_sink(log_stdout);
    Config cfg;
    cfg.load(default_config_path());
    if (cfg.get("server_url").empty()) {
        fprintf(stderr, "Run `nxplex-cli login` and `nxplex-cli use <n>` first.\n");
        return 2;
    }
    if (!net_init()) return 1;
    PlexClient client;
    client.client_id = cfg.get("client_id");
    client.platform = "Linux";
    client.device = "PC";
    client.device_name = "NXPlex apptest";

    SimAudio audio;
    SimVideo video;
    int volume_seen = -1;
    App::Hooks hooks;
    hooks.set_volume = [&](int v) { volume_seen = v; };
    App app(cfg, client, audio, video, hooks);
    g_app = &app;
    CHECK(app.start(), "app started");
    Thread disp;
    disp.start(display_loop, nullptr, 0x20000);

    // ---- browse: libraries, then Continue Watching
    uint32_t seq = app.cmd_browse("/library/sections", 0);
    app.wait_idle(15000);
    NxpBrowse* b = new NxpBrowse();
    app.get_browse(*b);
    CHECK(b->seq == seq && b->state == NxpBrowse_Ready && b->count > 0, "libraries listed (%u)", b->count);
    for (uint32_t i = 0; i < b->count; i++) printf("      - %s [%s]\n", b->items[i].title, b->items[i].key);

    std::string key = argc > 1 ? argv[1] : "";
    if (key.empty()) {
        app.cmd_browse("/library/onDeck", 0);
        app.wait_idle(15000);
        app.get_browse(*b);
        CHECK(b->state == NxpBrowse_Ready, "continue watching listed (%u items)", b->count);
        for (uint32_t i = 0; i < b->count && key.empty(); i++)
            if (b->items[i].is_leaf) key = b->items[i].key;
        if (key.empty()) {
            fprintf(stderr, "Nothing in Continue Watching; pass an item key, e.g. apptest /library/metadata/123\n");
            return 2;
        }
    }
    printf("      playing %s\n", key.c_str());
    // Remember the item's resume point so the test can put it back afterwards.
    app.cmd_browse(key, 0);
    app.wait_idle(10000);
    app.get_browse(*b);
    uint32_t orig_offset = b->count ? b->items[0].view_offset_ms : 0;
    uint32_t orig_duration = b->count ? b->items[0].duration_ms : 0;

    // ---- play
    app.cmd_play(key, true);
    CHECK(wait_for("playing", 20000, [](const NxpStatus& s) { return s.play_state == NxpPlay_Playing; }),
          "stream started");
    sleep_ms(4000);
    NxpStatus s1 = status();
    CHECK(s1.position_ms >= 2500 && s1.position_ms <= 6000, "position advances from 0 (%u ms after ~4 s)", s1.position_ms);
    CHECK(video.shown > 60 && video.h == 216, "frames shown: %d at %dx%d", video.shown, video.w, video.h);
    printf("      title='%s' subtitle='%s' duration=%u\n", s1.title, s1.subtitle, s1.duration_ms);

    // ---- seek forward 60 s (stream restart at an offset)
    app.cmd_seek_relative(60000);
    CHECK(wait_for("seek", 15000, [](const NxpStatus& s) { return s.play_state == NxpPlay_Playing; }), "seek finished");
    sleep_ms(2000);
    NxpStatus s2 = status();
    int64_t want = (int64_t)s1.position_ms + 60000;
    CHECK(s2.position_ms >= want - 1500 && s2.position_ms <= want + 6000, "position after +60 s: %u (expected ~%lld+)",
          s2.position_ms, (long long)want);

    // ---- pause
    app.cmd_set_paused(1);
    sleep_ms(500);
    uint32_t p0 = status().position_ms;
    sleep_ms(2500);
    NxpStatus s3 = status();
    CHECK(s3.play_state == NxpPlay_Paused && s3.position_ms - p0 < 400, "paused holds position (%u -> %u)", p0,
          s3.position_ms);
    app.cmd_set_paused(0);
    sleep_ms(2000);
    CHECK(status().position_ms > s3.position_ms + 1000, "resumed");

    // ---- quality change restarts at the same spot with bigger frames
    uint32_t before_q = status().position_ms;
    app.cmd_set_quality(2);
    CHECK(wait_for("quality restart", 15000,
                   [](const NxpStatus& s) { return s.play_state == NxpPlay_Playing && s.quality == 2; }),
          "quality change");
    sleep_ms(2500);
    NxpStatus s4 = status();
    CHECK(video.h == 288 && s4.position_ms + 1500 >= before_q, "now %dx%d, position %u (was %u)", video.w, video.h,
          s4.position_ms, before_q);
    app.cmd_set_quality(1);
    app.cmd_set_volume(70);
    CHECK(volume_seen == 70, "volume hook");

    // ---- stop; Plex should now have a resume point near where we stopped
    uint32_t stop_at = status().position_ms;
    app.cmd_stop();
    CHECK(wait_for("stop", 10000, [](const NxpStatus& s) { return s.play_state == NxpPlay_Stopped; }), "stopped");
    sleep_ms(1000);
    app.cmd_browse(key, 0);
    app.wait_idle(10000);
    app.get_browse(*b);
    uint32_t vo = b->count ? b->items[0].view_offset_ms : 0;
    CHECK(vo + 15000 >= stop_at && vo <= stop_at + 5000, "Plex resume point %u ms (stopped at %u)", vo, stop_at);

    // ---- error path: bogus item
    app.cmd_play("/library/metadata/999999999", false);
    app.wait_idle(10000);
    NxpStatus s5 = status();
    CHECK(s5.play_state == NxpPlay_Stopped && s5.error[0], "bad item reports an error: '%s'", s5.error);

    g_run = false;
    disp.join();
    app.stop();

    // Restore the resume point we overwrote (the test played from the start).
    {
        PlexServerSession ss;
        ss.base_url = cfg.get("server_url");
        ss.token = cfg.get("server_token");
        std::string rk = key.substr(key.rfind('/') + 1);
        HttpRequest req = server_request(client, ss,
                                         "/:/timeline?ratingKey=" + rk + "&key=" + url_encode(key) +
                                             "&state=stopped&time=" + std::to_string(orig_offset) +
                                             "&duration=" + std::to_string(orig_duration));
        HttpResponse resp;
        http_fetch(req, resp, nullptr);
        printf("      restored resume point to %u ms (HTTP %d)\n", orig_offset, resp.status);
    }
    delete b;
    net_exit();
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
