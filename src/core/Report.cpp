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
    Severity worst = Severity::Pass;
    for (const auto& i : issues) {
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
