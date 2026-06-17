#pragma once

#include <string>
#include <utility>
#include <vector>

namespace argus {

enum class Severity {
    Pass = 0,   // check ran, nothing wrong
    Info = 1,   // informational only
    Warn = 2,   // probably a problem, engineer should look
    Fail = 3,   // hard failure / spec violation
};

const char* severityLabel(Severity s);

// One finding from one check. A check may emit several issues (e.g. one per dropout)
// plus a PASS summary when clean.
struct Issue {
    std::string check;     // category label, e.g. "Dropout", "Loudness"
    Severity severity = Severity::Pass;
    std::string summary;   // friendly one-liner
    std::string detail;    // longer plain-English description (optional)

    int channel = -1;      // -1 = whole file / not channel-specific
    double tStart = -1.0;  // seconds, -1 = not time-localised
    double tEnd = -1.0;

    // Extra point-in-time markers for a finding that recurs (e.g. every click). Drawn on
    // the spectrogram/waveform in addition to tStart. Empty for single-region findings.
    std::vector<double> marks;

    // Technical key/value pairs shown in the engineer-facing table.
    std::vector<std::pair<std::string, std::string>> fields;

    Issue& field(const std::string& k, const std::string& v) {
        fields.emplace_back(k, v);
        return *this;
    }
    bool localised() const { return tStart >= 0.0; }
};

}  // namespace argus
