#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace argus {

// Deinterleaved float32 audio. data[channel][frame], samples normalised to [-1, 1].
struct AudioBuffer {
    int sampleRate = 0;
    int channels = 0;
    std::vector<std::vector<float>> data;

    std::size_t frames() const { return data.empty() ? 0 : data[0].size(); }
    double durationSec() const {
        return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0;
    }
    bool empty() const { return frames() == 0; }
};

// Technical metadata describing the file as it sits on disk.
struct FileMetadata {
    std::string path;
    std::string filename;
    std::string container;   // "WAV", "AIFF", "FLAC", ...
    std::string codec;       // "PCM signed 24-bit", "FLAC", "MPEG-1 Layer III", ...
    int bitDepth = 0;        // bits per sample; 0 if not meaningful (compressed)
    int sampleRate = 0;      // Hz
    int channels = 0;
    std::uint64_t frames = 0;
    double durationSec = 0.0;
    bool lossless = true;
    std::uint64_t fileBytes = 0;
};

}  // namespace argus
