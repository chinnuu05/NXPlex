// The end-to-end tests change real watch progress (and restore it after).
// Running them while someone is watching would fight with that playback, so
// they refuse to start while the server reports an active session.
#pragma once
#include <cstdio>
#include <string>

#include "json.hpp"
#include "plex.hpp"

namespace nxp {

inline bool server_has_active_playback(const PlexClient& c, const PlexServerSession& s, std::string& what) {
    struct Sink : JsonSink {
        std::string what;
        int sessions = 0;
        void on_json(JsonEv ev, int depth, const std::string& key, const std::string& val) override {
            if (depth == 3 && ev == JsonEv::ObjBegin) sessions++;
            if (depth == 4 && ev == JsonEv::String && (key == "grandparentTitle" || key == "title") && what.size() < 120)
                what += (what.empty() ? "" : " / ") + val;
        }
    } sink;
    JsonParser p(sink);
    HttpRequest req = server_request(c, s, "/status/sessions");
    HttpResponse resp;
    bool ok = http_fetch_cb(req, resp, [&](const char* d, size_t n) { return p.feed(d, n); });
    what = sink.what;
    return ok && resp.status == 200 && sink.sessions > 0;
}

// Returns false (and explains) if the tests shouldn't run now.
inline bool safe_to_test(const PlexClient& c, const PlexServerSession& s) {
    std::string what;
    if (server_has_active_playback(c, s, what)) {
        fprintf(stderr,
                "Something is playing on this Plex server right now (%s).\n"
                "These tests change watch progress, so they won't run while anyone is watching.\n",
                what.c_str());
        return false;
    }
    return true;
}

}  // namespace nxp
