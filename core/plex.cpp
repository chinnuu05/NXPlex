#include "plex.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "json.hpp"
#include "platform.hpp"

namespace nxp {

static const char* kPlexTv = "https://plex.tv";
static const char* kClientsPlexTv = "https://clients.plex.tv";

void PlexClient::add_headers(Headers& h, const std::string& token) const {
    h.emplace_back("Accept", "application/json");
    h.emplace_back("X-Plex-Client-Identifier", client_id);
    h.emplace_back("X-Plex-Product", product);
    h.emplace_back("X-Plex-Version", version);
    h.emplace_back("X-Plex-Platform", platform);
    h.emplace_back("X-Plex-Device", device);
    h.emplace_back("X-Plex-Device-Name", device_name);
    h.emplace_back("X-Plex-Provides", "player");
    if (!token.empty()) h.emplace_back("X-Plex-Token", token);
}

std::string new_client_id() {
    uint8_t b[16];
    random_bytes(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    char s[40];
    snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

// ---------------------------------------------------------------- helpers

namespace {

// Adapts a lambda to JsonSink and exposes the parser for key_at().
struct FnSink : JsonSink {
    std::function<void(JsonEv, int, const std::string&, const std::string&)> fn;
    void on_json(JsonEv ev, int depth, const std::string& key, const std::string& val) override {
        fn(ev, depth, key, val);
    }
};

std::string http_err(const HttpResponse& r, const char* what) {
    if (!r.error.empty()) return r.error;
    char b[96];
    if (r.status == 401) snprintf(b, sizeof(b), "%s: not authorized (sign in again)", what);
    else snprintf(b, sizeof(b), "%s: HTTP %d", what, r.status);
    return b;
}

// Runs req and feeds a 2xx body to the parser via sink. Returns true when the
// status is 2xx and the JSON is complete.
bool fetch_json(const HttpRequest& req, HttpResponse& resp, JsonParser& parser, const char* what, std::string& err) {
    bool ok_status = false;
    bool ok = http_fetch_cb(req, resp, [&](const char* d, size_t n) {
        if (!ok_status) ok_status = resp.status >= 200 && resp.status < 300;
        if (!ok_status) return true;  // drain error bodies silently
        return parser.feed(d, n);
    });
    if (!ok || resp.status < 200 || resp.status >= 300) {
        err = http_err(resp, what);
        return false;
    }
    if (!parser.finish()) {
        err = std::string(what) + ": unexpected response format";
        return false;
    }
    return true;
}

HttpRequest tv_request(const PlexClient& c, const std::string& url, const std::string& token) {
    HttpRequest r;
    r.url = url;
    c.add_headers(r.headers, token);
    return r;
}

uint32_t to_u32(const std::string& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 10); }

}  // namespace

// ---------------------------------------------------------------- sign-in

bool pin_create(const PlexClient& c, PlexPin& pin, std::string& err) {
    HttpRequest req = tv_request(c, std::string(kPlexTv) + "/api/v2/pins?strong=false", "");
    req.method = "POST";
    pin = PlexPin();
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        if (depth != 1) return;
        if (key == "id" && ev == JsonEv::Number) pin.id = strtoull(val.c_str(), nullptr, 10);
        else if (key == "code" && ev == JsonEv::String) pin.code = val;
        else if (key == "expiresIn" && ev == JsonEv::Number) pin.expires_in_s = to_u32(val);
    };
    JsonParser parser(sink);
    HttpResponse resp;
    if (!fetch_json(req, resp, parser, "plex.tv sign-in", err)) return false;
    if (!pin.id || pin.code.empty()) {
        err = "plex.tv sign-in: no code returned";
        return false;
    }
    return true;
}

PinStatus pin_check(const PlexClient& c, const PlexPin& pin, std::string& token_out, std::string& err) {
    HttpRequest req = tv_request(c, std::string(kPlexTv) + "/api/v2/pins/" + std::to_string(pin.id), "");
    token_out.clear();
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        if (depth == 1 && key == "authToken" && ev == JsonEv::String) token_out = val;
    };
    JsonParser parser(sink);
    HttpResponse resp;
    if (!fetch_json(req, resp, parser, "plex.tv sign-in", err)) {
        if (resp.error.empty() && resp.status == 404) return PinStatus::Expired;
        return PinStatus::Error;
    }
    return token_out.empty() ? PinStatus::Pending : PinStatus::Linked;
}

bool account_name(const PlexClient& c, const std::string& token, std::string& name, std::string& err) {
    HttpRequest req = tv_request(c, std::string(kPlexTv) + "/api/v2/user", token);
    std::string username, title;
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        if (depth != 1 || ev != JsonEv::String) return;
        if (key == "username") username = val;
        else if (key == "title") title = val;
    };
    JsonParser parser(sink);
    HttpResponse resp;
    if (!fetch_json(req, resp, parser, "plex.tv account", err)) return false;
    name = !title.empty() ? title : username;
    return true;
}

// ---------------------------------------------------------------- servers

bool list_servers(const PlexClient& c, std::vector<PlexServer>& out, std::string& err) {
    out.clear();
    HttpRequest req =
        tv_request(c, std::string(kClientsPlexTv) + "/api/v2/resources?includeHttps=1&includeRelay=1&includeIPv6=0",
                   c.user_token);
    // [ {name, provides, owned, accessToken, connections: [ {...} ] } ]
    PlexServer cur;
    PlexConnection conn;
    bool is_server = false;
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        if (depth == 1 && ev == JsonEv::ObjBegin) {
            cur = PlexServer();
            is_server = false;
        } else if (depth == 1 && ev == JsonEv::ObjEnd) {
            if (is_server) out.push_back(cur);
        } else if (depth == 2) {
            if (key == "name") cur.name = val;
            else if (key == "clientIdentifier") cur.machine_id = val;
            else if (key == "accessToken" && ev == JsonEv::String) cur.access_token = val;
            else if (key == "productVersion") cur.product_version = val;
            else if (key == "owned") cur.owned = val == "true";
            else if (key == "presence") cur.presence = val == "true";
            else if (key == "provides") is_server = ("," + val + ",").find(",server,") != std::string::npos;
        } else if (depth == 3 && ev == JsonEv::ObjBegin) {
            conn = PlexConnection();
        } else if (depth == 3 && ev == JsonEv::ObjEnd) {
            cur.connections.push_back(conn);
        } else if (depth == 4) {
            if (key == "uri") conn.uri = val;
            else if (key == "protocol") conn.protocol = val;
            else if (key == "address") conn.address = val;
            else if (key == "port") conn.port = (uint16_t)to_u32(val);
            else if (key == "local") conn.local = val == "true";
            else if (key == "relay") conn.relay = val == "true";
        }
    };
    JsonParser parser(sink);
    HttpResponse resp;
    return fetch_json(req, resp, parser, "plex.tv server list", err);
}

bool probe_server(const PlexClient& c, const std::string& base_url, const std::string& token, std::string* name_out,
                  std::string& err, uint32_t timeout_ms) {
    HttpRequest req;
    req.url = base_url + "/";
    req.timeout_ms = timeout_ms;
    req.max_redirects = 0;
    c.add_headers(req.headers, token);
    std::string name;
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        if (depth == 2 && key == "friendlyName" && ev == JsonEv::String) name = val;
    };
    JsonParser parser(sink);
    HttpResponse resp;
    if (!fetch_json(req, resp, parser, "server", err)) return false;
    if (name_out) *name_out = name;
    return true;
}

bool pick_connection(const PlexClient& c, const PlexServer& s, std::string& base_url, std::string& err,
                     std::vector<std::string>* log) {
    // Candidate order: local https, local plain http (same address), remote, relay.
    std::vector<std::pair<std::string, uint32_t>> cands;
    auto add = [&](const std::string& u, uint32_t t) {
        for (auto& e : cands)
            if (e.first == u) return;
        if (!u.empty()) cands.emplace_back(u, t);
    };
    for (auto& k : s.connections)
        if (k.local && !k.relay) add(k.uri, 2500);
    for (auto& k : s.connections)
        if (k.local && !k.relay && !k.address.empty() && k.port)
            add("http://" + k.address + ":" + std::to_string(k.port), 2500);
    for (auto& k : s.connections)
        if (!k.local && !k.relay) add(k.uri, 4000);
    for (auto& k : s.connections)
        if (k.relay) add(k.uri, 6000);

    std::string last;
    for (auto& cand : cands) {
        std::string e;
        uint64_t t0 = now_ms();
        bool ok = probe_server(c, cand.first, s.access_token, nullptr, e, cand.second);
        if (log) {
            char b[64];
            snprintf(b, sizeof(b), " (%ums)", (unsigned)(now_ms() - t0));
            log->push_back((ok ? "ok    " : "fail  ") + cand.first + b + (ok ? "" : ": " + e));
        }
        if (ok) {
            base_url = cand.first;
            return true;
        }
        last = e;
    }
    err = cands.empty() ? "server has no connections" : "server unreachable (" + last + ")";
    return false;
}

// ---------------------------------------------------------------- browsing

bool PlexItem::is_leaf() const {
    if (is_section) return false;
    return type == "track" || type == "episode" || type == "movie" || type == "clip";
}

HttpRequest server_request(const PlexClient& c, const PlexServerSession& s, const std::string& path_and_query) {
    HttpRequest req;
    req.url = s.base_url + path_and_query;
    c.add_headers(req.headers, s.token);
    return req;
}

bool next_episode(const PlexClient& c, const PlexServerSession& s, const PlexItem& cur, PlexItem& next,
                  std::string& err) {
    err.clear();
    if (cur.type != "episode" || cur.parent_rating_key.empty()) return false;
    // 1. Later episode in the same season.
    PlexPage season;
    if (!browse(c, s, "/library/metadata/" + cur.parent_rating_key + "/children", 0, 500, season, err)) return false;
    const PlexItem* best = nullptr;
    for (auto& e : season.items) {
        if (e.type != "episode" || e.rating_key == cur.rating_key || e.index <= cur.index) continue;
        if (!best || e.index < best->index) best = &e;
    }
    if (best) {
        next = *best;
        return true;
    }
    // 2. First episode of the next season (specials, season 0, don't continue into season 1).
    if (cur.grandparent_rating_key.empty() || cur.parent_index < 1) return false;
    PlexPage seasons;
    if (!browse(c, s, "/library/metadata/" + cur.grandparent_rating_key + "/children", 0, 500, seasons, err))
        return false;
    const PlexItem* ns = nullptr;
    for (auto& z : seasons.items) {
        if (z.type != "season" || z.index <= cur.parent_index) continue;
        if (!ns || z.index < ns->index) ns = &z;
    }
    if (!ns) return false;
    PlexPage eps;
    if (!browse(c, s, ns->key, 0, 500, eps, err)) return false;
    for (auto& e : eps.items) {
        if (e.type != "episode") continue;
        if (!best || e.index < best->index) best = &e;
    }
    if (!best) return false;
    next = *best;
    return true;
}

bool transcode_decision(const PlexClient& c, const PlexServerSession& s, const std::string& query, std::string& err) {
    HttpRequest req = server_request(c, s, "/video/:/transcode/universal/decision?" + query);
    req.timeout_ms = 15000;
    int code = 0;
    std::string text;
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        if (depth != 2) return;
        if (key == "generalDecisionCode") code = atoi(val.c_str());
        else if (key == "generalDecisionText" && ev == JsonEv::String) text = val;
        else if (key == "transcodeDecisionText" && ev == JsonEv::String && code >= 2000) text = val;
    };
    JsonParser parser(sink);
    HttpResponse resp;
    if (!fetch_json(req, resp, parser, "transcode decision", err)) return false;
    if (code >= 2000) {  // 1xxx = OK, 2xxx/3xxx/4xxx = can't play
        err = text.empty() ? "server can't convert this item" : text;
        return false;
    }
    return true;
}

bool browse(const PlexClient& c, const PlexServerSession& s, const std::string& path_in, uint32_t start,
            uint32_t count, PlexPage& page, std::string& err) {
    std::string path = path_in.empty() ? "/library/sections" : path_in;
    std::string base_path = path.substr(0, path.find('?'));
    bool sections = base_path == "/library/sections";
    std::string url = path + (path.find('?') == std::string::npos ? "?" : "&") +
                      "X-Plex-Container-Start=" + std::to_string(start) +
                      "&X-Plex-Container-Size=" + std::to_string(count);
    HttpRequest req = server_request(c, s, url);

    page = PlexPage();
    page.offset = start;
    PlexItem it;
    std::string title1, title2;
    uint32_t size = 0;
    bool have_total = false;
    int media_i = -1, part_i = -1;
    JsonParser* parser = nullptr;
    FnSink sink;
    sink.fn = [&](JsonEv ev, int depth, const std::string& key, const std::string& val) {
        // {MediaContainer:{..., Metadata|Directory:[ {item} ]}}
        if (depth == 2) {
            if (key == "totalSize") { page.total = to_u32(val); have_total = true; }
            else if (key == "size") size = to_u32(val);
            else if (key == "offset") page.offset = to_u32(val);
            else if (key == "title1") title1 = val;
            else if (key == "title2") title2 = val;
            return;
        }
        if (depth == 3 && (ev == JsonEv::ObjBegin || ev == JsonEv::ObjEnd)) {
            const std::string& arr = parser->key_at(2);
            if (arr != "Metadata" && arr != "Directory") return;
            if (ev == JsonEv::ObjBegin) {
                it = PlexItem();
                media_i = part_i = -1;
                return;
            }
            if (sections) {
                it.is_section = true;
                it.key = "/library/sections/" + it.key + "/all";
            } else if (!it.key.empty() && it.key[0] != '/') {
                it.key = base_path + "/" + it.key;
            }
            if (!it.title.empty() && !it.key.empty()) page.items.push_back(it);
            return;
        }
        if (depth == 4) {
            if (key == "type") it.type = val;
            else if (key == "title") it.title = val;
            else if (key == "parentTitle") it.parent = val;
            else if (key == "grandparentTitle") it.grandparent = val;
            else if (key == "key") it.key = val;
            else if (key == "ratingKey") it.rating_key = val;
            else if (key == "parentRatingKey") it.parent_rating_key = val;
            else if (key == "grandparentRatingKey") it.grandparent_rating_key = val;
            else if (key == "duration") it.duration_ms = to_u32(val);
            else if (key == "viewOffset") it.view_offset_ms = to_u32(val);
            else if (key == "leafCount") it.leaf_count = to_u32(val);
            else if (key == "viewedLeafCount") it.viewed_leaf_count = to_u32(val);
            else if (key == "viewCount") it.view_count = to_u32(val);
            else if (key == "index") it.index = atoi(val.c_str());
            else if (key == "parentIndex") it.parent_index = atoi(val.c_str());
            else if (key == "year") it.year = atoi(val.c_str());
            return;
        }
        // First Media element, first Part.
        if (depth == 5 && ev == JsonEv::ObjBegin && parser->key_at(4) == "Media") media_i++;
        if (media_i != 0 || parser->key_at(4) != "Media") return;
        if (depth == 6) {
            if (key == "container") it.container = val;
            else if (key == "audioCodec") it.audio_codec = val;
            else if (key == "videoCodec") it.video_codec = val;
            else if (key == "duration" && !it.duration_ms) it.duration_ms = to_u32(val);
            return;
        }
        if (depth == 7 && ev == JsonEv::ObjBegin && parser->key_at(6) == "Part") part_i++;
        if (depth == 8 && part_i == 0 && parser->key_at(6) == "Part" && key == "key") it.part_key = val;
    };
    JsonParser p(sink);
    parser = &p;
    HttpResponse resp;
    if (!fetch_json(req, resp, p, "library", err)) return false;
    if (!have_total) page.total = page.offset + (size ? size : (uint32_t)page.items.size());
    page.title = !title2.empty() ? title1 + " - " + title2 : title1;
    return true;
}

}  // namespace nxp
