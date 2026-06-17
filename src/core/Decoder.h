#pragma once

#include <string>

#include "AudioBuffer.h"

namespace argus {

struct DecodeResult {
    bool ok = false;
    std::string error;
    AudioBuffer buffer;
    FileMetadata meta;
};

// Decode any libsndfile-supported file (WAV/AIFF/FLAC/CAF, plus MP3/Ogg/Opus where
// the linked libsndfile was built with them) into deinterleaved float samples.
DecodeResult decodeFile(const std::string& path);

}  // namespace argus
