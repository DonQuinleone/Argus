#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace argus {

// Format a value with a fixed number of decimals (printf-style, no locale surprises).
inline std::string fmt(double v, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

inline std::string fmtInt(long long v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

// Seconds -> "h:mm:ss.mmm" (matches the timecode style engineers read in RX).
inline std::string timecode(double sec) {
    if (sec < 0) return "-";
    long long ms = static_cast<long long>(std::llround(sec * 1000.0));
    long long h = ms / 3600000;
    ms %= 3600000;
    long long m = ms / 60000;
    ms %= 60000;
    long long s = ms / 1000;
    ms %= 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%03lld", h, m, s, ms);
    return buf;
}

// Linear amplitude (0..1) -> dBFS. Returns -inf-ish floor for silence.
inline double toDb(double amp) {
    if (amp <= 1e-12) return -200.0;
    return 20.0 * std::log10(amp);
}

inline std::string fmtDb(double db) {
    if (db <= -199.0) return "-inf";
    return fmt(db, 1) + " dBFS";
}

}  // namespace argus
