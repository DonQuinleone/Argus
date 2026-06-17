// Lightweight audio audition engine built on miniaudio. Holds an interleaved copy
// of the currently-selected file and plays a [start, end] region with optional
// looping, exposing an atomic playhead position for the UI to draw.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/AudioBuffer.h"

struct ma_device;

namespace argus {

class Player {
public:
    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Replace the loaded audio (stops playback). Safe to call repeatedly; the
    // previous device is torn down before the sample buffer is swapped.
    void load(const AudioBuffer& buf);
    void clear();
    bool hasAudio() const { return sampleRate_ > 0 && !samples_.empty(); }

    // Play [startSec, endSec). If endSec <= startSec, plays to end of file.
    void play(double startSec, double endSec);
    void togglePause();
    void stop();

    void setLoop(bool v) { loop_.store(v); }
    bool loop() const { return loop_.load(); }

    bool playing() const { return playing_.load(); }
    bool paused() const { return paused_.load(); }
    double positionSec() const {
        return sampleRate_ > 0 ? static_cast<double>(pos_.load()) / sampleRate_ : 0.0;
    }
    double durationSec() const {
        return (sampleRate_ > 0 && channels_ > 0)
                   ? static_cast<double>(samples_.size() / channels_) / sampleRate_
                   : 0.0;
    }

private:
    static void dataCallback(ma_device* dev, void* out, const void* in, std::uint32_t frames);
    void teardown();
    void ensureDevice();

    ma_device* device_ = nullptr;
    std::vector<float> samples_;  // interleaved, [-1, 1]
    int channels_ = 0;
    int sampleRate_ = 0;

    std::atomic<std::uint64_t> pos_{0};          // absolute frame index
    std::atomic<std::uint64_t> regionStart_{0};  // frame
    std::atomic<std::uint64_t> regionEnd_{0};    // frame (exclusive)
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> loop_{false};
};

}  // namespace argus
