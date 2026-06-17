// Fallback decoder for compressed formats libsndfile cannot open (AAC / Apple
// Lossless in .m4a/.mp4/.aac containers). Implemented per platform with the OS
// codec stack (CoreAudio on macOS, Media Foundation on Windows) or FFmpeg on
// Linux. Exactly one implementation is compiled; when none is available a stub
// returns false and the format is reported as unsupported.
#pragma once

#include "../Decoder.h"

namespace argus {

// Decode `path` into `out` (meta + deinterleaved float buffer). Returns true on
// success; false if this build cannot decode the format (caller then reports the
// original libsndfile error).
bool decodeCompressed(const std::string& path, DecodeResult& out);

}  // namespace argus
