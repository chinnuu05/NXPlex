// Plex API: plex.tv PIN sign-in (plex.tv/link), server discovery, library
// browsing. Everything streams through the SAX parser; nothing keeps a whole
// response in memory.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "net.hpp"

namespace nxp {

struct PlexClient {
    std::string client_id;  // stable per install (X-Plex-Client-Identifier)
    std::string product = "NXPlex";
    std::string version = "0.1.0";
    std::string platform;
    std::string device;
    std::string device_name;
    std::string user_token;  // plex.tv account token (empty = signed out)

    void add_headers(Headers& h, const std::string& token) const;
};

std::string new_client_id();  // random UUID v4

// ---- sign-in -------------------------------------------------------------
struct PlexPin {
    uint64_t id = 0;
    std::string code;  // 4 characters, entered at https://plex.tv/link
    uint32_t expires_in_s = 0;
};
bool pin_create(const PlexClient& c, PlexPin& pin, std::string& err);
enum class PinStatus { Pending, Linked, Expired, Error };
PinStatus pin_check(const PlexClient& c, const PlexPin& pin, std::string& token_out, std::string& err);
// Validates a token; returns the account's display name.
bool account_name(const PlexClient& c, const std::string& token, std::string& name, std::string& err);

// ---- servers -------------------------------------------------------------
struct PlexConnection {
    std::string uri, protocol, address;
    uint16_t port = 0;
    bool local = false, relay = false;
};
struct PlexServer {
    std::string name, machine_id, access_token, product_version;
    bool owned = false, presence = false;
    std::vector<PlexConnection> connections;
};
bool list_servers(const PlexClient& c, std::vector<PlexServer>& out, std::string& err);
// Probes the server's connections (local first, relay last) and returns the
// first base URL that answers. `log` (optional) receives one line per attempt.
bool pick_connection(const PlexClient& c, const PlexServer& s, std::string& base_url, std::string& err,
                     std::vector<std::string>* log = nullptr);
// Checks a manually configured base URL (e.g. http://192.168.1.10:32400).
bool probe_server(const PlexClient& c, const std::string& base_url, const std::string& token,
                  std::string* name_out, std::string& err, uint32_t timeout_ms = 3000);

// ---- browsing ------------------------------------------------------------
struct PlexItem {
    std::string type;  // section types: movie/show/artist/photo; item types: track, episode, ...
    std::string title, parent, grandparent;
    std::string key;         // what to browse/play next (absolute path)
    std::string rating_key;
    std::string part_key;    // first media part (direct file), leaves only
    std::string container, audio_codec, video_codec;
    uint32_t duration_ms = 0, view_offset_ms = 0;
    uint32_t leaf_count = 0, viewed_leaf_count = 0, view_count = 0;
    int index = -1, parent_index = -1, year = 0;
    bool is_section = false;
    bool is_leaf() const;  // playable item rather than a folder
};
struct PlexPage {
    std::string title;
    uint32_t offset = 0, total = 0;
    std::vector<PlexItem> items;
};
struct PlexServerSession {
    std::string base_url;  // http(s)://host:port, no trailing slash
    std::string token;
};
bool browse(const PlexClient& c, const PlexServerSession& s, const std::string& path, uint32_t start,
            uint32_t count, PlexPage& page, std::string& err);

// Asks the server whether it can transcode with these universal-transcode
// query parameters. Must precede /start for the same session, or Plex
// answers the start with HTTP 400.
bool transcode_decision(const PlexClient& c, const PlexServerSession& s, const std::string& query, std::string& err);

// Adds X-Plex-Token etc. to an arbitrary server path; used by the streaming code.
HttpRequest server_request(const PlexClient& c, const PlexServerSession& s, const std::string& path_and_query);

}  // namespace nxp
