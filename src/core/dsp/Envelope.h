#pragma once

#include <cstddef>
#include <vector>

#include "../AudioBuffer.h"

namespace argus {

// Short-time amplitude envelope of a mono downmix of the audio.
struct Envelope {
    int sampleRate = 0;
    int hop = 0;  // samples between frames
    int win = 0;  // window length in samples
    std::vector<float> rms;   // linear RMS per frame
    std::vector<float> peak;  // linear peak per frame

    std::size_t size() const { return rms.size(); }
    // Centre time of frame i, in seconds.
    double timeAt(std::size_t i) const {
        return sampleRate > 0 ? (static_cast<double>(i) * hop + win * 0.5) / sampleRate : 0.0;
    }
};

// Downmix all channels to mono (simple average) into `out`.
void downmixMono(const AudioBuffer& buf, std::vector<float>& out);

// Compute the RMS/peak envelope of a mono signal with the given window/hop in ms.
Envelope computeEnvelope(const AudioBuffer& buf, double winMs, double hopMs);

// Same, but from an already-prepared mono signal (e.g. a filtered band).
Envelope computeEnvelopeMono(const std::vector<float>& mono, int sampleRate, double winMs,
                             double hopMs);

// In-place 2nd-order (RBJ) high-pass at cutoffHz, Q=0.707. Used to isolate the
// high-frequency noise floor for dropout detection.
void highpass(std::vector<float>& x, int sampleRate, double cutoffHz);

}  // namespace argus
