// episodetest: checks "Next episode" and autoplay against a real server.
// Picks the first episode in Continue Watching that has a following episode,
// then restores the watched state and resume points it touched.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "app.hpp"
#include "platform.hpp"
#include "sim.hpp"
#include "testguard.hpp"

using namespace nxp;

static int g_fail = 0;
#define CHECK(cond, ...)                       \
    do {                                       \
        bool ok_ = (cond);                     \
        printf("%s  ", ok_ ? "PASS" : "FAIL"); \
        printf(__VA_ARGS__);                   \
        printf("\n");                          \
        if (!ok_) g_fail++;                    \
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

static std::string episode_label(const PlexItem& e) {
    char b[160];
    snprintf(b, sizeof(b), "S%d E%d - %s", e.parent_index, e.index, e.title.c_str());
    return b;
}

// Waits until the status subtitle shows `want` and the episode is playing.
static bool wait_playing(const std::string& want, uint32_t ms) {
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        NxpStatus s = status();
        if (s.play_state == NxpPlay_Playing && want == s.subtitle) return true;
        sleep_ms(200);
    }
    NxpStatus s = status();
    printf("      (waited for '%s'; got state=%d subtitle='%s' error='%s')\n", want.c_str(), s.play_state, s.subtitle,
           s.error);
    return false;
}

static void log_stdout(const char* l) { printf("      [log] %s\n", l); }

int main() {
    log_set_sink(log_stdout);
    Config cfg;
    cfg.load(default_config_path());
    if (cfg.get("server_url").empty()) {
        fprintf(stderr, "Run `nxplex-cli login` and `nxplex-cli use <n>` first.\n");
        return 2;
    }
    net_init();
    PlexClient client;
    client.client_id = cfg.get("client_id");
    client.platform = "Generic";
    client.device = "PC";
    client.device_name = "NXPlex episodetest";
    PlexServerSession ss{cfg.get("server_url"), cfg.get("server_token")};
    if (!safe_to_test(client, ss)) return 2;

    // ---- pick an episode with a next one, and remember both episodes' state
    PlexPage deck;
    std::string err;
    browse(client, ss, "/library/onDeck", 0, 50, deck, err);
    PlexItem cur, next;
    bool found = false;
    for (auto& it : deck.items) {
        if (it.type != "episode") continue;
        PlexPage one;
        if (!browse(client, ss, it.key, 0, 1, one, err) || one.items.empty()) continue;
        cur = one.items[0];
        cur.key = it.key;
        if (next_episode(client, ss, cur, next, err)) {
            found = true;
            break;
        }
    }
    if (!found) {
        printf("SKIP  no episode with a following episode in Continue Watching\n");
        return 0;
    }
    PlexPage np;
    browse(client, ss, next.key, 0, 1, np, err);
    if (!np.items.empty()) next = np.items[0], next.key = np.items[0].key;
    printf("      current: %s %s (offset %u, views %u)\n", cur.grandparent.c_str(), episode_label(cur).c_str(),
           cur.view_offset_ms, cur.view_count);
    printf("      next:    %s (offset %u, views %u)\n", episode_label(next).c_str(), next.view_offset_ms,
           next.view_count);
    CHECK(next.parent_index > cur.parent_index || (next.parent_index == cur.parent_index && next.index > cur.index),
          "next_episode() goes forward");

    SimAudio audio;
    SimVideo video;
    App app(cfg, client, audio, video, App::Hooks());
    g_app = &app;
    app.start();
    app.cmd_set_autoplay(true);
    Thread disp;
    disp.start(display_loop, nullptr, 0x20000);

    // ---- "Next episode" button
    app.cmd_play(cur.key, true);
    CHECK(wait_playing(episode_label(cur), 20000), "current episode playing");
    CHECK(status().is_episode == 1, "status reports an episode");
    sleep_ms(2000);
    app.cmd_next_episode();
    CHECK(wait_playing(episode_label(next), 20000), "Next episode -> %s", episode_label(next).c_str());
    app.cmd_stop();
    app.wait_idle(10000);

    // ---- autoplay: play the current episode's last seconds
    app.cmd_play(cur.key, true);
    wait_playing(episode_label(cur), 20000);
    app.cmd_seek_absolute(cur.duration_ms > 12000 ? cur.duration_ms - 12000 : 0);
    sleep_ms(3000);
    printf("      position %u of %u, waiting for the end...\n", status().position_ms, status().duration_ms);
    CHECK(wait_playing(episode_label(next), 60000), "autoplay started %s", episode_label(next).c_str());
    PlexPage after;
    browse(client, ss, cur.key, 0, 1, after, err);
    CHECK(!after.items.empty() && after.items[0].view_count > cur.view_count, "finished episode marked watched");
    app.cmd_stop();
    app.wait_idle(10000);

    // ---- autoplay off: nothing starts at the end
    app.cmd_set_autoplay(false);
    app.cmd_play(cur.key, true);
    wait_playing(episode_label(cur), 20000);
    app.cmd_seek_absolute(cur.duration_ms > 10000 ? cur.duration_ms - 10000 : 0);
    uint64_t end = now_ms() + 45000;
    while (now_ms() < end && status().play_state != NxpPlay_Stopped) sleep_ms(250);
    CHECK(status().play_state == NxpPlay_Stopped, "autoplay off: stops at the end");
    {
        PlexPage w;
        browse(client, ss, cur.key, 0, 1, w, err);
        CHECK(!w.items.empty() && w.items[0].view_count > cur.view_count, "...and marks it watched");
    }
    app.cmd_set_autoplay(true);

    g_run = false;
    disp.join();
    app.stop();

    // ---- restore watched state and resume points
    for (const PlexItem* e : {&cur, &next}) {
        HttpResponse r;
        if (e->view_count == 0) {
            HttpRequest q = server_request(client, ss, "/:/unscrobble?key=" + e->rating_key +
                                                           "&identifier=com.plexapp.plugins.library");
            http_fetch(q, r, nullptr);
        }
        if (e->view_offset_ms) {
            HttpRequest q = server_request(client, ss,
                                           "/:/timeline?ratingKey=" + e->rating_key + "&key=" + url_encode(e->key) +
                                               "&state=stopped&time=" + std::to_string(e->view_offset_ms) +
                                               "&duration=" + std::to_string(e->duration_ms));
            http_fetch(q, r, nullptr);
        }
        PlexPage chk;
        browse(client, ss, e->key, 0, 1, chk, err);
        bool ok = !chk.items.empty() && (e->view_count > 0 || chk.items[0].view_count == 0) &&
                  (chk.items[0].view_offset_ms / 1000 == e->view_offset_ms / 1000);
        printf("%s  restored %s: views %u, offset %u (originally %u, %u)\n", ok ? "PASS" : "WARN",
               episode_label(*e).c_str(), chk.items.empty() ? 0 : chk.items[0].view_count,
               chk.items.empty() ? 0 : chk.items[0].view_offset_ms, e->view_count, e->view_offset_ms);
    }
    net_exit();
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
