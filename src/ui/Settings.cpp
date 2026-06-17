#include "Settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace argus {
namespace {

constexpr const char* kBundleId = "com.donquinleone.argus";

// Encode/decode arbitrary text (notes may contain newlines) for the key=value
// line format: backslash-escape '\' and newlines.
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

}  // namespace

std::string stateFilePath() {
    namespace fs = std::filesystem;
    fs::path dir = configDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "state.ini").string();
}

bool loadState(AppState& out) {
    std::ifstream f(stateFilePath());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        auto vi = [&] { return std::atoi(v.c_str()); };
        auto vd = [&] { return std::atof(v.c_str()); };
        if (k == "colormap") out.colormap = vi();
        else if (k == "freqScale") out.freqScale = vi();
        else if (k == "fftSize") out.fftSize = vi();
        else if (k == "dbLow") out.dbLow = vd();
        else if (k == "dbHigh") out.dbHigh = vd();
        else if (k == "winX") out.winX = vi();
        else if (k == "winY") out.winY = vi();
        else if (k == "winW") out.winW = vi();
        else if (k == "winH") out.winH = vi();
        else if (k == "listWidth") out.listWidth = vd();
        else if (k == "listCollapsed") out.listCollapsed = vi() != 0;
        else if (k == "showSettings") out.showSettings = vi() != 0;
        else if (k == "showReportInfo") out.showReportInfo = vi() != 0;
        else if (k == "theme") out.theme = vi();
        else if (k == "profile") out.profile = vi();
        else if (k == "preroll") out.preroll = vd();
        else if (k == "postroll") out.postroll = vd();
        else if (k == "loop") out.loop = vi() != 0;
        else if (k == "lastFolder") out.lastFolder = v;
        else if (k == "qaEngineer") out.reportInfo.qaEngineer = dec(v);
        else if (k == "qaContact") out.reportInfo.contact = dec(v);
        else if (k == "qaStudio") out.reportInfo.studio = dec(v);
        else if (k == "qaClient") out.reportInfo.client = dec(v);
        else if (k == "qaProject") out.reportInfo.project = dec(v);
        else if (k == "qaCatalog") out.reportInfo.catalog = dec(v);
        else if (k == "qaNotes") out.reportInfo.notes = dec(v);
        else if (k == "qaShowEngineer") out.reportInfo.showEngineer = vi() != 0;
        else if (k == "qaShowContact") out.reportInfo.showContact = vi() != 0;
        else if (k == "qaShowStudio") out.reportInfo.showStudio = vi() != 0;
        else if (k == "qaShowClient") out.reportInfo.showClient = vi() != 0;
        else if (k == "qaShowProject") out.reportInfo.showProject = vi() != 0;
        else if (k == "qaShowCatalog") out.reportInfo.showCatalog = vi() != 0;
        else if (k == "qaShowNotes") out.reportInfo.showNotes = vi() != 0;
        else if (k == "qaShowDate") out.reportInfo.showDate = vi() != 0;
        else if (k == "qaShowLogo") out.reportInfo.showLogo = vi() != 0;
    }
    return true;
}

bool saveState(const AppState& st) {
    std::ofstream f(stateFilePath(), std::ios::trunc);
    if (!f) return false;
    f << "colormap=" << st.colormap << "\n"
      << "freqScale=" << st.freqScale << "\n"
      << "fftSize=" << st.fftSize << "\n"
      << "dbLow=" << st.dbLow << "\n"
      << "dbHigh=" << st.dbHigh << "\n"
      << "winX=" << st.winX << "\n"
      << "winY=" << st.winY << "\n"
      << "winW=" << st.winW << "\n"
      << "winH=" << st.winH << "\n"
      << "listWidth=" << st.listWidth << "\n"
      << "listCollapsed=" << (st.listCollapsed ? 1 : 0) << "\n"
      << "showSettings=" << (st.showSettings ? 1 : 0) << "\n"
      << "showReportInfo=" << (st.showReportInfo ? 1 : 0) << "\n"
      << "theme=" << st.theme << "\n"
      << "profile=" << st.profile << "\n"
      << "preroll=" << st.preroll << "\n"
      << "postroll=" << st.postroll << "\n"
      << "loop=" << (st.loop ? 1 : 0) << "\n"
      << "lastFolder=" << st.lastFolder << "\n";
    const ReportInfo& ri = st.reportInfo;
    f << "qaEngineer=" << enc(ri.qaEngineer) << "\n"
      << "qaContact=" << enc(ri.contact) << "\n"
      << "qaStudio=" << enc(ri.studio) << "\n"
      << "qaClient=" << enc(ri.client) << "\n"
      << "qaProject=" << enc(ri.project) << "\n"
      << "qaCatalog=" << enc(ri.catalog) << "\n"
      << "qaNotes=" << enc(ri.notes) << "\n"
      << "qaShowEngineer=" << (ri.showEngineer ? 1 : 0) << "\n"
      << "qaShowContact=" << (ri.showContact ? 1 : 0) << "\n"
      << "qaShowStudio=" << (ri.showStudio ? 1 : 0) << "\n"
      << "qaShowClient=" << (ri.showClient ? 1 : 0) << "\n"
      << "qaShowProject=" << (ri.showProject ? 1 : 0) << "\n"
      << "qaShowCatalog=" << (ri.showCatalog ? 1 : 0) << "\n"
      << "qaShowNotes=" << (ri.showNotes ? 1 : 0) << "\n"
      << "qaShowDate=" << (ri.showDate ? 1 : 0) << "\n"
      << "qaShowLogo=" << (ri.showLogo ? 1 : 0) << "\n";
    return true;
}

}  // namespace argus
