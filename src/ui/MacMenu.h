// Native macOS menu bar (NSMenu) for argus. No-op / unused on other platforms.
#pragma once

#if defined(__APPLE__)
#include <functional>
#include <string>

namespace argus {

// Absolute path to a resource inside the .app bundle, or "" if not found.
std::string macResourcePath(const std::string& name);

// Callbacks invoked (on the main thread) when the corresponding menu item fires.
struct MacMenuCallbacks {
    std::function<void()> openFile;
    std::function<void()> openFolder;
    std::function<void()> exportPdf;
    std::function<void()> exportCsv;
    std::function<void()> exportJson;
    std::function<void()> exportAll;
    std::function<void()> exportBatchPdf;
    std::function<void()> toggleSettings;
    std::function<void()> toggleReportInfo;
    std::function<void()> toggleTheme;
    std::function<void()> quit;  // request a clean shutdown (saves state, joins threads)
};

// Build and install the application menu. Standard items (About/Hide/Quit) use
// AppKit's own actions; custom items dispatch to the supplied callbacks.
void installMacMenu(const MacMenuCallbacks& cb);

}  // namespace argus
#endif
