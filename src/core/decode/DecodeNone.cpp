// Fallback used when no compressed-audio decoder is available for this build
// (non-Apple, non-Windows host without FFmpeg). AAC/ALAC are then unsupported and
// the caller reports the original libsndfile error.
#include "PlatformDecode.h"

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(ARGUS_HAVE_FFMPEG)
namespace argus {

bool decodeCompressed(const std::string&, DecodeResult&) { return false; }

}  // namespace argus
#endif
