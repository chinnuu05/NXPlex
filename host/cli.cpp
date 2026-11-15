// nxplex-cli: desktop test harness for the NXPlex core (the same code the
// Switch sysmodule runs). Sign in, pick a server, browse, dump raw API calls.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../core/config.hpp"
#include "../core/net.hpp"
#include "../core/platform.hpp"
#include "../core/plex.hpp"

using namespace nxp;

static Config g_cfg;
static PlexClient g_client;

static void log_stderr(const char* line) { fprintf(stderr, "[log] %s\n", line); }

static int usage() {
    fprintf(stderr,
            "usage: nxplex-cli <command>\n"
            "  login                 sign in with a code at plex.tv/link\n"
            "  logout                forget the saved sign-in\n"
            "  whoami                show the signed-in account\n"
            "  servers               list your Plex servers\n"
            "  use <n> [url]         select server n (optionally force a base URL)\n"
            "  ls [n|path]           browse: libraries, item n of the last listing, or a path\n"
            "  raw <path>            print the raw JSON for a server path\n"
            "  save <path> <file>    download a server path (e.g. a stream) to a file\n");
    return 2;
}

static bool require_login() {
    if (!g_client.user_token.empty()) return true;
    fprintf(stderr, "Not signed in. Run: nxplex-cli login\n");
    return false;
}

static bool require_server(PlexServerSession& s) {
    if (!require_login()) return false;
    s.base_url = g_cfg.get("server_url");
    s.token = g_cfg.get("server_token");
    if (s.base_url.empty() || s.token.empty()) {
        fprintf(stderr, "No server selected. Run: nxplex-cli servers, then nxplex-cli use <n>\n");
        return false;
    }
    return true;
}

static int cmd_login() {
    PlexPin pin;
    std::string err;
    if (!pin_create(g_client, pin, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    printf("Go to  https://plex.tv/link  and enter:  %s\n", pin.code.c_str());
    printf("Waiting (code expires in %u s)...\n", pin.expires_in_s);
    fflush(stdout);
    uint64_t deadline = now_ms() + (uint64_t)(pin.expires_in_s ? pin.expires_in_s : 300) * 1000;
    int errors = 0;
    while (now_ms() < deadline) {
        sleep_ms(2000);
        std::string token;
        PinStatus st = pin_check(g_client, pin, token, err);
        if (st == PinStatus::Linked) {
            std::string name;
            if (!account_name(g_client, token, name, err)) {
                fprintf(stderr, "error: %s\n", err.c_str());
                return 1;
            }
            g_cfg.set("user_token", token);
            g_cfg.set("account", name);
            if (!g_cfg.save()) {
                fprintf(stderr, "error: could not write %s\n", g_cfg.path().c_str());
                return 1;
            }
            printf("Signed in as %s. Saved to %s\n", name.c_str(), g_cfg.path().c_str());
            return 0;
        }
        if (st == PinStatus::Expired) break;
        if (st == PinStatus::Error && ++errors >= 5) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
    }
    fprintf(stderr, "The code expired. Run login again.\n");
    return 1;
}

static int cmd_logout() {
    for (const char* k : {"user_token", "account", "server_url", "server_token", "server_name", "server_id"})
        g_cfg.erase(k);
    g_cfg.save();
    printf("Signed out.\n");
    return 0;
}

static int cmd_whoami() {
    if (!require_login()) return 1;
    std::string name, err;
    if (!account_name(g_client, g_client.user_token, name, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    printf("%s\n", name.c_str());
    std::string srv = g_cfg.get("server_name");
    if (!srv.empty()) printf("server: %s (%s)\n", srv.c_str(), g_cfg.get("server_url").c_str());
    return 0;
}

static int cmd_servers() {
    if (!require_login()) return 1;
    std::vector<PlexServer> list;
    std::string err;
    if (!list_servers(g_client, list, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (list.empty()) printf("No servers on this account.\n");
    std::string sel = g_cfg.get("server_id");
    for (size_t i = 0; i < list.size(); i++) {
        auto& s = list[i];
        printf("%c %zu. %s%s%s  (v%s, %zu connections)\n", s.machine_id == sel ? '*' : ' ', i + 1, s.name.c_str(),
               s.owned ? "" : " [shared]", s.presence ? "" : " [offline]", s.product_version.c_str(),
               s.connections.size());
        for (auto& k : s.connections)
            printf("       %s%s%s\n", k.uri.c_str(), k.local ? " [local]" : "", k.relay ? " [relay]" : "");
    }
    return 0;
}

static int cmd_use(int argc, char** argv) {
    if (argc < 1) return usage();
    if (!require_login()) return 1;
    std::vector<PlexServer> list;
    std::string err;
    if (!list_servers(g_client, list, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    size_t n = strtoul(argv[0], nullptr, 10);
    if (n < 1 || n > list.size()) {
        fprintf(stderr, "No server %s (see: nxplex-cli servers)\n", argv[0]);
        return 1;
    }
    PlexServer& s = list[n - 1];
    std::string base;
    if (argc >= 2) {
        base = argv[1];
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string name;
        if (!probe_server(g_client, base, s.access_token, &name, err)) {
            fprintf(stderr, "error: %s did not answer: %s\n", base.c_str(), err.c_str());
            return 1;
        }
    } else {
        std::vector<std::string> log;
        printf("Finding a working connection to %s...\n", s.name.c_str());
        bool ok = pick_connection(g_client, s, base, err, &log);
        for (auto& l : log) printf("  %s\n", l.c_str());
        if (!ok) {
            fprintf(stderr, "error: %s\nTip: pass the address yourself: nxplex-cli use %zu http://<ip>:32400\n",
                    err.c_str(), n);
            return 1;
        }
    }
    g_cfg.set("server_id", s.machine_id);
    g_cfg.set("server_name", s.name);
    g_cfg.set("server_url", base);
    g_cfg.set("server_token", s.access_token);
    g_cfg.save();
    printf("Using %s at %s\n", s.name.c_str(), base.c_str());
    return 0;
}

static std::string last_listing_path() { return g_cfg.path() + ".last"; }

static std::string fmt_dur(uint32_t ms) {
    char b[24];
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
    return b;
}

static int cmd_ls(int argc, char** argv) {
    PlexServerSession s;
    if (!require_server(s)) return 1;
    std::string path = "/library/sections";
    if (argc >= 1) {
        if (argv[0][0] == '/') {
            path = argv[0];
        } else {
            // Item n of the last listing.
            int want = atoi(argv[0]);
            FILE* f = fopen(last_listing_path().c_str(), "r");
            char line[1024];
            int i = 0;
            bool found = false;
            while (f && fgets(line, sizeof(line), f)) {
                if (++i == want) {
                    line[strcspn(line, "\r\n")] = 0;
                    path = line;
                    found = true;
                    break;
                }
            }
            if (f) fclose(f);
            if (!found) {
                fprintf(stderr, "No item %s in the last listing.\n", argv[0]);
                return 1;
            }
        }
    }
    PlexPage page;
    std::string err;
    if (!browse(g_client, s, path, 0, 200, page, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    printf("== %s  [%s]  %zu of %u\n", page.title.c_str(), path.c_str(), page.items.size(), page.total);
    FILE* f = fopen(last_listing_path().c_str(), "w");
    for (size_t i = 0; i < page.items.size(); i++) {
        auto& it = page.items[i];
        std::string extra;
        if (it.is_leaf()) {
            extra = "  " + fmt_dur(it.duration_ms);
            if (it.view_offset_ms) extra += " (resume " + fmt_dur(it.view_offset_ms) + ")";
            extra += "  " + it.container + "/" + (it.video_codec.empty() ? "" : it.video_codec + "+") + it.audio_codec;
            if (!it.part_key.empty()) extra += "  part=" + it.part_key;
        } else if (it.leaf_count) {
            extra = "  (" + std::to_string(it.leaf_count) + ")";
        }
        std::string label = it.title;
        if (it.type == "episode")
            label = it.grandparent + " S" + std::to_string(it.parent_index) + "E" + std::to_string(it.index) + " " +
                    it.title;
        else if (it.type == "track" && !it.grandparent.empty())
            label = it.grandparent + " - " + it.title;
        printf("%3zu. [%s] %s%s\n", i + 1, it.type.c_str(), label.c_str(), extra.c_str());
        if (f) fprintf(f, "%s\n", it.key.c_str());
    }
    if (f) fclose(f);
    return 0;
}

static int cmd_raw(int argc, char** argv, const char* save_to) {
    if (argc < 1) return usage();
    PlexServerSession s;
    if (!require_server(s)) return 1;
    HttpRequest req = server_request(g_client, s, argv[0]);
    req.timeout_ms = 30000;
    HttpResponse resp;
    FILE* out = stdout;
    if (save_to && !(out = fopen(save_to, "wb"))) {
        fprintf(stderr, "cannot write %s\n", save_to);
        return 1;
    }
    size_t total = 0;
    bool ok = http_fetch_cb(req, resp, [&](const char* d, size_t n) {
        total += n;
        return fwrite(d, 1, n, out) == n;
    });
    if (save_to) fclose(out);
    fprintf(stderr, "HTTP %d, %s, %zu bytes%s%s\n", resp.status, resp.content_type.c_str(), total,
            resp.error.empty() ? "" : ", error: ", resp.error.c_str());
    return ok && resp.status >= 200 && resp.status < 300 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (getenv("NXPLEX_DEBUG")) log_set_sink(log_stderr);
    if (argc < 2) return usage();
    g_cfg.load(default_config_path());
    if (g_cfg.get("client_id").empty()) {
        g_cfg.set("client_id", new_client_id());
        g_cfg.save();
    }
    g_client.client_id = g_cfg.get("client_id");
    g_client.platform = getenv("NXPLEX_PLATFORM") ? getenv("NXPLEX_PLATFORM") : "Linux";  // debug override
    g_client.device = "PC";
    g_client.device_name = "NXPlex CLI";
    g_client.user_token = g_cfg.get("user_token");
    if (!net_init()) {
        fprintf(stderr, "TLS init failed\n");
        return 1;
    }
    std::string cmd = argv[1];
    int rc;
    if (cmd == "login") rc = cmd_login();
    else if (cmd == "logout") rc = cmd_logout();
    else if (cmd == "whoami") rc = cmd_whoami();
    else if (cmd == "servers") rc = cmd_servers();
    else if (cmd == "use") rc = cmd_use(argc - 2, argv + 2);
    else if (cmd == "ls") rc = cmd_ls(argc - 2, argv + 2);
    else if (cmd == "raw") rc = cmd_raw(argc - 2, argv + 2, nullptr);
    else if (cmd == "save" && argc >= 4) rc = cmd_raw(1, argv + 2, argv[3]);
    else rc = usage();
    net_exit();
    return rc;
}
