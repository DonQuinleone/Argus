// Native macOS menu bar (NSMenu) for argus. No-op / unused on other platforms.
#pragma once

#if defined(__APPLE__)
#include <functional>
#include <string>
#include <vector>

namespace argus {

// Absolute path to a resource inside the .app bundle, or "" if not found.
std::string macResourcePath(const std::string& name);

// Native Cocoa file dialogs. Each activates the app first so the panel is focused and
// in front (the osascript path used by tinyfiledialogs does not on modern macOS).
// Extensions are bare ("wav", "pdf"); empty list = any. Return "" / empty on cancel.
std::vector<std::string> macOpenFiles(const std::string& title,
                                      const std::vector<std::string>& exts, bool multiple);
std::string macOpenFolder(const std::string& title, const std::string& defaultDir);
std::string macSaveFile(const std::string& title, const std::string& defaultPath,
                        const std::string& ext);

// Callbacks invoked (on the main thread) when the corresponding menu item fires.
struct MacMenuCallbacks {
    std::function<void()> openFile;
    std::function<void()> openFolder;
    std::function<void()> exportPdf;
    std::function<void()> exportCsv;
    std::function<void()> exportJson;
    std::function<void()> exportSpecPng;
    std::function<void()> exportAll;
    std::function<void()> exportBatchPdf;
    std::function<void()> toggleSettings;
    std::function<void()> toggleReportInfo;
    std::function<void()> toggleProfiles;
    std::function<void()> toggleTheme;
    std::function<void()> quit;  // request a clean shutdown (saves state, joins threads)
};

// Build and install the application menu. Standard items (About/Hide/Quit) use
// AppKit's own actions; custom items dispatch to the supplied callbacks.
void installMacMenu(const MacMenuCallbacks& cb);

}  // namespace argus
#endif
