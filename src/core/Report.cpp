#include "Report.h"

namespace argus {

const char* severityLabel(Severity s) {
    switch (s) {
        case Severity::Pass: return "PASS";
        case Severity::Info: return "INFO";
        case Severity::Warn: return "WARN";
        case Severity::Fail: return "FAIL";
    }
    return "?";
}

Severity Report::verdict() const {
    // The overall verdict reflects only actionable findings: a file is PASS unless
    // something warns or fails. Info-level findings (e.g. the loudness readout) are
    // purely informational and must not drag a clean file down to INFO. Per-finding
    // labels and the info count are reported separately.
    Severity worst = Severity::Pass;
    for (const auto& i : issues) {
        if (i.severity != Severity::Warn && i.severity != Severity::Fail) continue;
        if (static_cast<int>(i.severity) > static_cast<int>(worst)) worst = i.severity;
    }
    return worst;
}

int Report::count(Severity s) const {
    int n = 0;
    for (const auto& i : issues)
        if (i.severity == s) ++n;
    return n;
}

}  // namespace argus
