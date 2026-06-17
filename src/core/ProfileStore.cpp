#include "ProfileStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace argus {
namespace {

constexpr const char* kBundleId = "com.donquinleone.argus";

// Per-user config directory (mirrors src/ui/Settings.cpp so the GUI and CLI agree).
std::string configDir() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    fs::path dir = base ? fs::path(base) : fs::path(".");
    dir /= kBundleId;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    fs::path dir = home ? fs::path(home) : fs::path(".");
    dir /= "Library/Application Support";
    dir /= kBundleId;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path dir = xdg ? fs::path(xdg) : (home ? fs::path(home) / ".config" : fs::path("."));
    dir /= kBundleId;
#endif
    return dir.string();
}

// Names are single-line; escape backslash and newline so they survive the line format.
std::string enc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') {}
        else o += c;
    }
    return o;
}
std::string dec(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += (n == 'n') ? '\n' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string ratesToStr(const std::vector<int>& rates) {
    std::string s;
    for (std::size_t i = 0; i < rates.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(rates[i]);
    }
    return s;
}

std::vector<int> ratesFromStr(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        int r = std::atoi(tok.c_str());
        if (r > 0) out.push_back(r);
    }
    return out;
}

}  // namespace

std::string customProfilesPath() {
    namespace fs = std::filesystem;
    fs::path dir = configDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "profiles.ini").string();
}

std::vector<Profile> loadCustomProfiles() {
    std::vector<Profile> out;
    std::ifstream f(customProfilesPath());
    if (!f) return out;
    std::string line;
    Profile cur;
    bool inProfile = false;
    auto flush = [&] {
        if (inProfile && !cur.name.empty()) out.push_back(cur);
        cur = Profile();
        inProfile = false;
    };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[profile]") {
            flush();
            inProfile = true;
            continue;
        }
        if (!inProfile) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        auto vi = [&] { return std::atoi(v.c_str()); };
        auto vd = [&] { return std::atof(v.c_str()); };
        if (k == "name") cur.name = dec(v);
        else if (k == "bitDepth") cur.requiredBitDepth = vi();
        else if (k == "channels") cur.requiredChannels = vi();
        else if (k == "lossless") cur.requireLossless = vi() != 0;
        else if (k == "rates") cur.approvedRates = ratesFromStr(v);
        else if (k == "ceiling") cur.truePeakCeiling = vd();
        else if (k == "enforceTP") cur.enforceTruePeak = vi() != 0;
        else if (k == "checkLufs") cur.checkLoudnessTarget = vi() != 0;
        else if (k == "lufsTarget") cur.lufsTarget = vd();
        else if (k == "lufsTol") cur.lufsTolerance = vd();
    }
    flush();
    return out;
}

bool saveCustomProfiles(const std::vector<Profile>& profiles) {
    std::ofstream f(customProfilesPath(), std::ios::trunc);
    if (!f) return false;
    for (const auto& p : profiles) {
        if (p.name.empty()) continue;
        f << "[profile]\n"
          << "name=" << enc(p.name) << "\n"
          << "bitDepth=" << p.requiredBitDepth << "\n"
          << "channels=" << p.requiredChannels << "\n"
          << "lossless=" << (p.requireLossless ? 1 : 0) << "\n"
          << "rates=" << ratesToStr(p.approvedRates) << "\n"
          << "ceiling=" << p.truePeakCeiling << "\n"
          << "enforceTP=" << (p.enforceTruePeak ? 1 : 0) << "\n"
          << "checkLufs=" << (p.checkLoudnessTarget ? 1 : 0) << "\n"
          << "lufsTarget=" << p.lufsTarget << "\n"
          << "lufsTol=" << p.lufsTolerance << "\n";
    }
    return true;
}

}  // namespace argus
