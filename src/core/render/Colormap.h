#pragma once

#include <algorithm>
#include <array>

namespace argus {

enum class Colormap {
    CyanOrange,  // RX-style "Cyan to orange" (default)
    Inferno,
    Grayscale,
};

namespace detail {
using Anchor = std::array<float, 3>;
template <std::size_t N>
inline void rampColor(const std::array<Anchor, N>& a, float t, unsigned char& r,
                      unsigned char& g, unsigned char& b) {
    t = std::min(1.0f, std::max(0.0f, t));
    float x = t * (N - 1);
    int i = static_cast<int>(x);
    if (i >= static_cast<int>(N) - 1) i = static_cast<int>(N) - 2;
    float f = x - i;
    r = static_cast<unsigned char>(a[i][0] + f * (a[i + 1][0] - a[i][0]));
    g = static_cast<unsigned char>(a[i][1] + f * (a[i + 1][1] - a[i][1]));
    b = static_cast<unsigned char>(a[i][2] + f * (a[i + 1][2] - a[i][2]));
}
}  // namespace detail

// Map normalised intensity t in [0,1] to RGB for the chosen colormap.
inline void colormap(Colormap cm, float t, unsigned char& r, unsigned char& g, unsigned char& b) {
    switch (cm) {
        case Colormap::CyanOrange: {
            // iZotope RX "Cyan to orange": a long black floor (so the recording's
            // noise floor reads black, not a blue wash), faint blue for low-level
            // content, then orange -> yellow -> white for the musical energy.
            static const std::array<detail::Anchor, 10> a = {{
                {0, 0, 0}, {0, 0, 0}, {6, 16, 52}, {22, 70, 140},
                {42, 124, 178}, {180, 110, 70}, {224, 128, 42}, {240, 170, 55},
                {248, 210, 110}, {255, 252, 238},
            }};
            detail::rampColor(a, t, r, g, b);
            return;
        }
        case Colormap::Inferno: {
            static const std::array<detail::Anchor, 8> a = {{
                {0, 0, 4}, {40, 11, 84}, {101, 21, 110}, {159, 42, 99},
                {212, 72, 66}, {245, 125, 21}, {250, 193, 39}, {252, 255, 164},
            }};
            detail::rampColor(a, t, r, g, b);
            return;
        }
        case Colormap::Grayscale:
        default: {
            unsigned char v = static_cast<unsigned char>(std::min(1.0f, std::max(0.0f, t)) * 255);
            r = g = b = v;
            return;
        }
    }
}

}  // namespace argus
