#include "ConsoleReport.h"

#include <cstdio>

#include "../core/Util.h"

namespace argus {
namespace {

const char* icon(Severity s) {
    switch (s) {
        case Severity::Pass: return "✅";  // ✅
        case Severity::Info: return "ℹ️ ";  // ℹ️
        case Severity::Warn: return "⚠️ ";  // ⚠️
        case Severity::Fail: return "❌";  // ❌
    }
    return "?";
}

}  // namespace

void printReport(const Report& rep) {
    std::printf("\n\U0001F3A7 %s\n", rep.meta.filename.c_str());

    if (!rep.ok()) {
        std::printf("  ❌ DECODE FAILED: %s\n", rep.decodeError.c_str());
        return;
    }

    for (const auto& is : rep.issues) {
        std::printf("\n%s [%s] %s", icon(is.severity), is.check.c_str(), is.summary.c_str());
        if (is.localised()) {
            std::printf("  @ %s", timecode(is.tStart).c_str());
            if (is.tEnd > is.tStart) std::printf("–%s", timecode(is.tEnd).c_str());
        }
        if (is.channel >= 0) std::printf("  (ch %d)", is.channel);
        std::printf("\n");
        if (!is.detail.empty()) std::printf("       %s\n", is.detail.c_str());
        for (const auto& kv : is.fields)
            std::printf("         - %s: %s\n", kv.first.c_str(), kv.second.c_str());
    }

    std::printf("\n   Verdict: %s   (%d fail, %d warn, %d info)\n",
                severityLabel(rep.verdict()), rep.count(Severity::Fail),
                rep.count(Severity::Warn), rep.count(Severity::Info));
}

}  // namespace argus
