#include <cmath>
#include <vector>

#include "../Util.h"
#include "Analyses.h"

namespace argus {

// Inter-channel correlation = mono compatibility. Strongly negative correlation
// means the stereo image partially cancels when summed to mono (a real problem for
// DSPs that fold down). Correlation of exactly +1 means dual-mono.
void analyzePhase(const AudioBuffer& buf, std::vector<Issue>& out) {
    if (buf.channels < 2) return;
    const auto& L = buf.data[0];
    const auto& R = buf.data[1];
    const std::size_t n = buf.frames();
    if (n == 0) return;

    long double sLR = 0, sLL = 0, sRR = 0;
    bool identical = true;
    for (std::size_t i = 0; i < n; ++i) {
        double l = L[i], r = R[i];
        sLR += l * r;
        sLL += l * l;
        sRR += r * r;
        if (identical && L[i] != R[i]) identical = false;
    }
    double denom = std::sqrt(static_cast<double>(sLL) * static_cast<double>(sRR));
    double corr = denom > 0 ? static_cast<double>(sLR) / denom : 0.0;

    // Fraction of time windows that are out of phase.
    const std::size_t win = static_cast<std::size_t>(buf.sampleRate * 0.1);  // 100 ms
    std::size_t outWin = 0, totWin = 0;
    for (std::size_t s = 0; s + win <= n; s += win) {
        long double a = 0, b = 0, cc = 0;
        for (std::size_t i = s; i < s + win; ++i) {
            a += L[i] * R[i];
            b += L[i] * L[i];
            cc += R[i] * R[i];
        }
        double d = std::sqrt(static_cast<double>(b) * static_cast<double>(cc));
        if (d > 0) {
            ++totWin;
            if (static_cast<double>(a) / d < -0.3) ++outWin;
        }
    }
    double outPct = totWin ? 100.0 * outWin / totWin : 0.0;

    Issue is;
    is.check = "Phase / mono";
    is.field("Correlation", fmt(corr, 3))
        .field("Out-of-phase time", fmt(outPct, 1) + " %");

    if (identical) {
        is.severity = Severity::Info;
        is.summary = "Dual mono (both channels identical)";
        is.detail = "Left and right are bit-identical - the file is effectively mono.";
    } else if (corr < -0.2) {
        is.severity = Severity::Warn;
        is.summary = "Predominantly out of phase (correlation " + fmt(corr, 2) + ")";
        is.detail = "The channels are strongly anti-correlated; a mono fold-down will lose "
                    "level or cancel. Check polarity/phase before delivery.";
    } else if (outPct > 20.0) {
        is.severity = Severity::Warn;
        is.summary = "Out of phase for " + fmt(outPct, 0) + "% of the programme";
        is.detail = "Significant stretches are anti-correlated; verify mono compatibility.";
    } else {
        is.severity = Severity::Pass;
        is.summary = "Mono-compatible (correlation " + fmt(corr, 2) + ")";
    }
    out.push_back(std::move(is));
}

}  // namespace argus
