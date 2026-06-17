#include <algorithm>
#include <cmath>
#include <vector>

#include "../Util.h"
#include "../dsp/Envelope.h"
#include "Analyses.h"

namespace argus {
namespace {

// A dropped / missing frame punches a *broadband hole* in the audio: the full-band level
// collapses to near the recording floor and snaps back, with abrupt edges. We require all
// three signatures together, because any one alone is fooled by ordinary music:
//   1. the high-frequency noise floor vanishes (HF band falls below the file's HF floor),
//   2. the *full-band* level collapses far below its local baseline (a real hole, not just
//      missing treble - sustained vowels and reverb tails lose HF while staying loud), and
//   3. the edges are sharp (the level a few ms either side is much higher), which a smooth
//      musical decay or reverb tail never is.
// This catches the reference defect (CATZ-0010, HF floor drops ~25 dB with a broadband hole)
// while rejecting the HF-only dips that flood acoustic material (e.g. choral masters).
constexpr double kWinMs = 6.0;             // short window keeps the dropout edges crisp
constexpr double kHopMs = 5.0;
constexpr double kHpCutoff = 12000.0;      // isolate the HF noise-floor band (least music here)
constexpr double kContentDb = -55.0;       // full-band level that counts as "content present"
constexpr double kFloorDropDb = 13.0;      // HF must fall this far below the noise floor
constexpr double kBroadbandDropDb = 12.0;  // full-band must collapse this far below its baseline
constexpr double kEdgeMs = 10.0;           // how far outside the hole we sample for the edge test
constexpr double kEdgeDropDb = 10.0;       // the level just outside must exceed the hole by this
constexpr double kMinDurMs = 12.0;
constexpr double kMaxDurMs = 800.0;
constexpr double kFailFullDropDb = 18.0;   // a near-total broadband collapse => Fail

inline double db(double lin) { return lin <= 1e-9 ? -180.0 : 20.0 * std::log10(lin); }

double percentile(std::vector<double> v, double pct) {
    if (v.empty()) return -180.0;
    std::size_t k = static_cast<std::size_t>(pct * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Centred median of `src` over +/-half samples.
std::vector<double> medianFilter(const std::vector<double>& src, int half) {
    const std::size_t n = src.size();
    std::vector<double> out(n);
    std::vector<double> tmp;
    tmp.reserve(2 * half + 1);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t lo = (i > static_cast<std::size_t>(half)) ? i - half : 0;
        std::size_t hi = std::min(n - 1, i + half);
        tmp.assign(src.begin() + lo, src.begin() + hi + 1);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        out[i] = tmp[tmp.size() / 2];
    }
    return out;
}

}  // namespace

void analyzeDropouts(const AudioBuffer& buf, std::vector<Issue>& out) {
    if (buf.sampleRate <= 0 || buf.frames() == 0) return;

    // Build a mono signal that never cancels. For correlated material mid =
    // (L+R)/2 has the best SNR; for out-of-phase material side = (L-R)/2 carries
    // the content. Analyse whichever has more energy.
    const std::size_t nf = buf.frames();
    std::vector<float> mono(nf);
    if (buf.channels >= 2) {
        const auto& L = buf.data[0];
        const auto& R = buf.data[1];
        double midE = 0, sideE = 0;
        for (std::size_t i = 0; i < nf; ++i) {
            double m = 0.5 * (L[i] + R[i]), s = 0.5 * (L[i] - R[i]);
            midE += m * m;
            sideE += s * s;
        }
        bool useSide = sideE > midE;
        for (std::size_t i = 0; i < nf; ++i)
            mono[i] = useSide ? 0.5f * (L[i] - R[i]) : 0.5f * (L[i] + R[i]);
    } else {
        mono = buf.data[0];
    }

    // High-frequency band envelope (the noise-floor band).
    std::vector<float> hp = mono;
    highpass(hp, buf.sampleRate, kHpCutoff);
    Envelope full = computeEnvelopeMono(mono, buf.sampleRate, kWinMs, kHopMs);
    Envelope hf = computeEnvelopeMono(hp, buf.sampleRate, kWinMs, kHopMs);

    const std::size_t n = std::min(full.size(), hf.size());
    if (n < 8) return;

    std::vector<double> fullDb(n), hfDb(n);
    for (std::size_t i = 0; i < n; ++i) {
        fullDb[i] = db(full.rms[i]);
        hfDb[i] = db(hf.rms[i]);
    }

    // Estimate the recording's HF noise floor from frames that contain content
    // (so leading/trailing silence doesn't drag the estimate down).
    std::vector<double> hfActive;
    hfActive.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        if (fullDb[i] > kContentDb) hfActive.push_back(hfDb[i]);
    if (hfActive.size() < n / 20) return;  // too little content to judge
    const double noiseFloor = percentile(hfActive, 0.05);

    // Context level (is there content around this instant?).
    const int half = std::max(
        4, static_cast<int>(std::lround(0.400 * buf.sampleRate / full.hop)) / 2);
    std::vector<double> fullBaseline = medianFilter(fullDb, half);

    // A frame belongs to a hole only if the HF floor has vanished *and* the full band has
    // collapsed well below its local baseline. Requiring the broadband collapse is what
    // rejects musical HF-only dips (sustained vowels, reverb tails), where the full-band
    // level barely changes (or even rises) while the treble fades.
    std::vector<char> dip(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (fullBaseline[i] <= kContentDb) continue;             // surrounded by content
        bool hfGone = hfDb[i] < noiseFloor - kFloorDropDb;       // HF floor vanished
        bool fullCollapsed = fullDb[i] < fullBaseline[i] - kBroadbandDropDb;  // real hole
        if (hfGone && fullCollapsed) dip[i] = 1;
    }

    const int edgeFrames = std::max(1, static_cast<int>(std::lround(kEdgeMs / kHopMs)));

    const double frameMs = 1000.0 * full.hop / full.sampleRate;
    int events = 0;

    std::size_t i = 0;
    while (i < n) {
        if (!dip[i]) { ++i; continue; }
        std::size_t j = i;
        while (j < n && dip[j]) ++j;  // dip run [i, j)
        double durMs = (j - i) * frameMs;

        if (durMs >= kMinDurMs && durMs <= kMaxDurMs) {
            double minHf = 1e9, minFull = 1e9, ctx = fullBaseline[i];
            for (std::size_t k = i; k < j; ++k) {
                minHf = std::min(minHf, hfDb[k]);
                minFull = std::min(minFull, fullDb[k]);
            }
            double hfDrop = noiseFloor - minHf;
            double fullDrop = ctx - minFull;

            // Sharp-edge test: the full-band level a few ms either side of the hole must sit
            // well above the hole's floor. A dropped frame snaps in/out; a musical decay or
            // reverb tail ramps gradually and fails this.
            std::size_t preIdx = i > static_cast<std::size_t>(edgeFrames) ? i - edgeFrames : 0;
            std::size_t postIdx = std::min(n - 1, (j - 1) + edgeFrames);
            double preLevel = fullDb[preIdx];
            double postLevel = fullDb[postIdx];
            bool sharpEdges = (preLevel - minFull >= kEdgeDropDb) &&
                              (postLevel - minFull >= kEdgeDropDb);
            if (!sharpEdges) { i = j; continue; }

            double tStart = full.timeAt(i);
            double tEnd = full.timeAt(j - 1);

            Issue is;
            is.check = "Dropout";
            is.severity = (fullDrop >= kFailFullDropDb) ? Severity::Fail : Severity::Warn;
            is.tStart = tStart;
            is.tEnd = tEnd;
            is.summary = "Audio dropout - noise floor vanishes for " + fmt(durMs, 0) + " ms";
            is.detail =
                "The recording's continuous high-frequency noise floor disappears here "
                "and returns abruptly, which is the signature of a dropped / missing audio "
                "frame or a digital glitch (a musical pause keeps the room/recording noise "
                "floor). Audition this point to confirm.";
            is.field("Start", timecode(tStart))
                .field("End", timecode(tEnd))
                .field("Duration", fmt(durMs, 0) + " ms")
                .field("HF floor drop", fmt(hfDrop, 1) + " dB below noise floor")
                .field("Broadband drop", fmt(fullDrop, 1) + " dB")
                .field("Noise floor (HF)", fmt(noiseFloor, 1) + " dB");
            out.push_back(std::move(is));
            ++events;
        }
        i = j;
    }

    if (events == 0) {
        Issue ok;
        ok.check = "Dropout";
        ok.severity = Severity::Pass;
        ok.summary = "No audio dropouts detected";
        out.push_back(std::move(ok));
    }
}

}  // namespace argus
