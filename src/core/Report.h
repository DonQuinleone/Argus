#pragma once

#include <vector>

#include "AudioBuffer.h"
#include "Issue.h"

namespace argus {

// A zoomed-in spectrogram raster around a single localised finding. Rendered in the
// worker (while the decoded buffer is still alive) so the UI dropdown and the PDF
// share one representation, mirroring how the full-file raster lives in Report.
struct CloseupView {
    int issueIndex = -1;        // index into Report::issues
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;  // RGBA8, row-major, top = high freq
    double winStart = 0.0;      // raster time span (seconds)
    double winEnd = 0.0;
    double evStart = 0.0;       // event region to box/highlight (seconds)
    double evEnd = 0.0;
    double minFreq = 0.0;       // Hz at bottom row
    double maxFreq = 0.0;       // Hz at top row
    int scale = 0;              // 0=Mel, 1=Log, 2=Linear (matches FreqScale)
    bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// Aggregated result for a single file. Owns the spectrogram raster once rendered
// (populated in M4) so the UI and exporters share one representation.
struct Report {
    FileMetadata meta;
    std::vector<Issue> issues;
    std::string decodeError;  // non-empty if the file could not be decoded

    // Spectrogram raster (RGBA8, row-major, top = high freq). Empty until rendered.
    int specWidth = 0;
    int specHeight = 0;
    std::vector<unsigned char> specRGBA;
    double specMinFreq = 0.0;   // Hz at bottom row
    double specMaxFreq = 0.0;   // Hz at top row
    bool specLogFreq = true;    // vertical axis is logarithmic in frequency
    int specScale = 0;          // 0=Mel, 1=Log, 2=Linear (matches FreqScale)
    double specDuration = 0.0;  // seconds spanned left..right

    // Downsampled peak-amplitude overview for the waveform plot (~2000 points).
    std::vector<float> overview;

    // Per-finding close-up rasters (one per localised Warn/Fail issue).
    std::vector<CloseupView> closeups;

    // The close-up for issue `issueIndex`, or nullptr if none was rendered.
    const CloseupView* closeupFor(int issueIndex) const {
        for (const auto& c : closeups)
            if (c.issueIndex == issueIndex) return &c;
        return nullptr;
    }

    // Worst severity across all issues; Pass if everything clean.
    Severity verdict() const;

    int count(Severity s) const;

    void add(Issue issue) { issues.push_back(std::move(issue)); }
    bool ok() const { return decodeError.empty(); }
};

}  // namespace argus
