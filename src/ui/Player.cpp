#include "Player.h"

#include <algorithm>
#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"

namespace argus {

Player::~Player() { teardown(); }

void Player::teardown() {
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
}

void Player::clear() {
    teardown();
    samples_.clear();
    channels_ = 0;
    sampleRate_ = 0;
    pos_.store(0);
    playing_.store(false);
    paused_.store(false);
}

void Player::load(const AudioBuffer& buf) {
    // Tear the device down first so the callback can't touch samples_ mid-swap.
    teardown();
    playing_.store(false);
    paused_.store(false);
    pos_.store(0);
    regionStart_.store(0);
    regionEnd_.store(0);

    channels_ = buf.channels;
    sampleRate_ = buf.sampleRate;
    const std::size_t frames = buf.frames();
    samples_.assign(frames * std::max(1, channels_), 0.0f);
    for (int c = 0; c < channels_; ++c) {
        const std::vector<float>& src = buf.data[c];
        for (std::size_t i = 0; i < frames; ++i)
            samples_[i * channels_ + c] = src[i];
    }
    if (hasAudio()) ensureDevice();
}

void Player::ensureDevice() {
    if (device_ || !hasAudio()) return;
    device_ = new ma_device();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = static_cast<ma_uint32>(channels_);
    cfg.sampleRate = static_cast<ma_uint32>(sampleRate_);
    cfg.dataCallback = &Player::dataCallback;
    cfg.pUserData = this;
    if (ma_device_init(nullptr, &cfg, device_) != MA_SUCCESS) {
        delete device_;
        device_ = nullptr;
        return;
    }
    // Leave the device running; the callback emits silence when not playing, which
    // avoids start/stop latency when auditioning short regions repeatedly.
    ma_device_start(device_);
}

void Player::play(double startSec, double endSec) {
    if (!hasAudio()) return;
    ensureDevice();
    const std::uint64_t total = samples_.size() / std::max(1, channels_);
    std::uint64_t s = static_cast<std::uint64_t>(std::max(0.0, startSec) * sampleRate_);
    std::uint64_t e = (endSec > startSec)
                          ? static_cast<std::uint64_t>(endSec * sampleRate_)
                          : total;
    s = std::min(s, total);
    e = std::min(std::max(e, s + 1), total);
    regionStart_.store(s);
    regionEnd_.store(e);
    pos_.store(s);
    paused_.store(false);
    playing_.store(true);
}

void Player::togglePause() {
    if (!hasAudio()) return;
    if (playing_.load()) {
        playing_.store(false);
        paused_.store(true);
    } else {
        // Resume; if nothing was loaded as a region, play whole file from cursor.
        if (regionEnd_.load() <= regionStart_.load()) {
            regionStart_.store(0);
            regionEnd_.store(samples_.size() / std::max(1, channels_));
        }
        paused_.store(false);
        playing_.store(true);
    }
}

void Player::stop() {
    playing_.store(false);
    paused_.store(false);
    pos_.store(regionStart_.load());
}

void Player::dataCallback(ma_device* dev, void* out, const void* /*in*/, std::uint32_t frames) {
    Player* self = static_cast<Player*>(dev->pUserData);
    float* dst = static_cast<float*>(out);
    const int ch = self->channels_;
    std::memset(dst, 0, static_cast<std::size_t>(frames) * ch * sizeof(float));
    if (!self->playing_.load()) return;

    const std::uint64_t end = self->regionEnd_.load();
    const std::uint64_t start = self->regionStart_.load();
    const bool loop = self->loop_.load();
    std::uint64_t pos = self->pos_.load();

    for (std::uint32_t i = 0; i < frames; ++i) {
        if (pos >= end) {
            if (loop) {
                pos = start;
            } else {
                self->playing_.store(false);
                break;
            }
        }
        const float* srcFrame = &self->samples_[pos * ch];
        for (int c = 0; c < ch; ++c) dst[i * ch + c] = srcFrame[c];
        ++pos;
    }
    self->pos_.store(pos);
}

}  // namespace argus
