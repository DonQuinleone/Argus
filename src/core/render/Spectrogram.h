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
    // Amplitude window mapped to the colour ramp. The default is biased low (top at
    // -8 dBFS) so mid/low-level detail and faint broadband transients (clicks, clipping
    // harmonics) render brightly rather than vanishing into the floor.
    double dbLow = -100.0;
    double dbHigh = -8.0;
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

// Render one channel's full spectrogram into an RGBA8 raster (for the Channels view).
void renderChannelSpectrogram(const std::vector<float>& chan, int sampleRate, int width,
                              int height, const SpectrogramSettings& settings,
                              std::vector<unsigned char>& rgba);

// Render a stereo goniometer (vectorscope) into a square RGBA8 raster: in-phase content
// runs vertically, anti-phase horizontally. Uses channels 0/1.
void renderGoniometer(const AudioBuffer& buf, int size, std::vector<unsigned char>& rgba);

// Inter-channel (0/1) Pearson correlation per `winSec` window, across the file.
void computeCorrelation(const AudioBuffer& buf, std::vector<float>& out, double winSec = 0.1);

// Render a per-channel DC-offset bar meter (signed bars from a centre = 0 line) into an
// RGBA8 raster. `dc` holds each channel's mean sample value.
void renderDcMeter(const std::vector<float>& dc, int w, int h, std::vector<unsigned char>& rgba);

}  // namespace argus
