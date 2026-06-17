// Persistent application state (spectrogram settings, window geometry, layout and
// audition preferences), stored as a small key=value file in the platform config
// directory keyed by the bundle identifier.
#pragma once

#include <string>

#include "core/ReportInfo.h"

namespace argus {

struct AppState {
    // Spectrogram settings (mirror SpectrogramSettings; stored as ints/doubles).
    int colormap = 0;    // Colormap::CyanOrange
    int freqScale = 0;   // FreqScale::Mel
    int fftSize = 2048;
    double dbLow = -100.0;
    double dbHigh = -8.0;

    // Window geometry (-1 = unset -> let the OS place it).
    int winX = -1, winY = -1, winW = 1500, winH = 950;

    // Layout.
    double listWidth = 280.0;
    bool showSettings = false;
    bool showReportInfo = false;
    int theme = 0;     // 0 = dark, 1 = light
    float accentR = 0.30f, accentG = 0.55f, accentB = 0.85f;  // UI accent colour
    int profile = 0;   // legacy: index into the delivery profiles (fallback)
    std::string profileName;  // selected profile by name (stable across custom-profile edits)

    // Audition.
    double preroll = 2.0, postroll = 2.0;
    bool loop = false;

    std::string lastFolder;

    // QA / sign-off details included on exported reports.
    ReportInfo reportInfo;
};

// Path to the state file (creates the parent directory if needed).
std::string stateFilePath();

// Load state from disk; returns false (leaving defaults) if no file exists.
bool loadState(AppState& out);

// Persist state to disk; returns false on write failure.
bool saveState(const AppState& st);

}  // namespace argus
