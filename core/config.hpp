// Flat key=value settings file (shared by the sysmodule and the overlay on
// Switch: sdmc:/config/nxplex/nxplex.ini).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace nxp {

class Config {
public:
    bool load(const std::string& path);  // missing file = empty config, returns true
    bool save() const;                    // atomic (write temp + rename)
    std::string get(const std::string& key, const std::string& def = "") const;
    void set(const std::string& key, const std::string& val);
    void erase(const std::string& key);
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::vector<std::pair<std::string, std::string>> kv_;
};

std::string default_config_path();

}  // namespace nxp
