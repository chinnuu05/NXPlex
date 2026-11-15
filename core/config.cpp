#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace nxp {

static void trim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    s.erase(0, i);
}

bool Config::load(const std::string& path) {
    path_ = path;
    kv_.clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return true;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        trim(l);
        if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        trim(k);
        trim(v);
        if (!k.empty()) set(k, v);
    }
    fclose(f);
    return true;
}

static void mkdirs_for(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/' && path[i - 1] != ':') mkdir(path.substr(0, i).c_str(), 0700);
    }
}

bool Config::save() const {
    if (path_.empty()) return false;
    mkdirs_for(path_);
    std::string tmp = path_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
#ifndef __SWITCH__
    chmod(tmp.c_str(), 0600);  // holds the Plex token
#endif
    fputs("# NXPlex settings. Holds your Plex sign-in token: keep it private.\n", f);
    bool ok = true;
    for (auto& e : kv_) ok = ok && fprintf(f, "%s=%s\n", e.first.c_str(), e.second.c_str()) > 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    remove(path_.c_str());  // rename() over an existing file fails on the Switch's FAT
    return rename(tmp.c_str(), path_.c_str()) == 0;
}

std::string Config::get(const std::string& key, const std::string& def) const {
    for (auto& e : kv_)
        if (e.first == key) return e.second;
    return def;
}

void Config::set(const std::string& key, const std::string& val) {
    std::string v = val;
    for (auto& c : v) if (c == '\n' || c == '\r') c = ' ';
    for (auto& e : kv_) {
        if (e.first == key) {
            e.second = v;
            return;
        }
    }
    kv_.emplace_back(key, v);
}

void Config::erase(const std::string& key) {
    for (size_t i = 0; i < kv_.size(); i++) {
        if (kv_[i].first == key) {
            kv_.erase(kv_.begin() + (long)i);
            return;
        }
    }
}

std::string default_config_path() {
#ifdef __SWITCH__
    return "sdmc:/config/nxplex/nxplex.ini";
#else
    const char* env = getenv("NXPLEX_CONFIG");
    if (env && *env) return env;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/nxplex/nxplex.ini";
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/nxplex/nxplex.ini";
#endif
}

}  // namespace nxp
