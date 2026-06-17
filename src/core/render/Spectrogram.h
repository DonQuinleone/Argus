#pragma once

#include "../AudioBuffer.h"
#include "../Report.h"
#include "Colormap.h"

namespace argus {

enum class FreqScale { Mel, Log, Linear };

// Spectrogram rendering options. Defaults mirror the engineer's iZotope RX preset
// (Hann window, Cyan-to-orange map, Mel scale, ~-103/+9 dB amplitude range).
struct SpectrogramSettings {
    int fftSize = 2048;           // power of two
    Colormap colormap = Colormap::CyanOrange;
    FreqScale freqScale = FreqScale::Mel;
    double dbLow = -103.4;         // amplitude range low (dBFS) -> bottom of colour ramp
    double dbHigh = 8.8;           // amplitude range high (dBFS) -> top of colour ramp
};

// Compute an STFT magnitude spectrogram and write an RGBA8 raster into the report
// (rep.specRGBA / specWidth / specHeight + axis metadata).
void renderSpectrogram(const AudioBuffer& buf, Report& rep, int targetWidth = 1200,
                       int height = 512, const SpectrogramSettings& settings = {});

// Render a zoomed-in close-up spectrogram around a single localised finding. The
// time window is the event span padded with context; the returned view records the
// raster plus the time/frequency axes and the event region to highlight. The
// caller is responsible for setting CloseupView::issueIndex.
CloseupView renderCloseup(const AudioBuffer& buf, const Issue& issue,
                          const SpectrogramSettings& settings = {});

}  // namespace argus
