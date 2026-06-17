#include "Decoder.h"

#include <sndfile.h>

#include <filesystem>
#include <vector>

#include "decode/PlatformDecode.h"

namespace argus {
namespace {

std::string majorFormatName(int format) {
    switch (format & SF_FORMAT_TYPEMASK) {
        case SF_FORMAT_WAV:   return "WAV";
        case SF_FORMAT_WAVEX: return "WAVE (extensible)";
        case SF_FORMAT_AIFF:  return "AIFF";
        case SF_FORMAT_FLAC:  return "FLAC";
        case SF_FORMAT_CAF:   return "CAF";
        case SF_FORMAT_OGG:   return "Ogg";
        case SF_FORMAT_W64:   return "Wave64";
        case SF_FORMAT_RF64:  return "RF64";
        case SF_FORMAT_MPEG:  return "MPEG";
        default:              return "Unknown";
    }
}

// Returns codec description + nominal bit depth + lossless flag.
struct SubtypeInfo {
    std::string codec;
    int bitDepth = 0;
    bool lossless = true;
};

SubtypeInfo subtypeInfo(int format) {
    switch (format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8: return {"PCM signed 8-bit", 8, true};
        case SF_FORMAT_PCM_U8: return {"PCM unsigned 8-bit", 8, true};
        case SF_FORMAT_PCM_16: return {"PCM signed 16-bit", 16, true};
        case SF_FORMAT_PCM_24: return {"PCM signed 24-bit", 24, true};
        case SF_FORMAT_PCM_32: return {"PCM signed 32-bit", 32, true};
        case SF_FORMAT_FLOAT:  return {"IEEE float 32-bit", 32, true};
        case SF_FORMAT_DOUBLE: return {"IEEE float 64-bit", 64, true};
        case SF_FORMAT_VORBIS: return {"Ogg Vorbis", 0, false};
        case SF_FORMAT_OPUS:   return {"Opus", 0, false};
        case SF_FORMAT_MPEG_LAYER_I:   return {"MPEG Layer I", 0, false};
        case SF_FORMAT_MPEG_LAYER_II:  return {"MPEG Layer II", 0, false};
        case SF_FORMAT_MPEG_LAYER_III: return {"MPEG Layer III (MP3)", 0, false};
        default:               return {"Unknown subtype", 0, true};
    }
}

}  // namespace

DecodeResult decodeFile(const std::string& path) {
    DecodeResult r;
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        r.error = "File not found: " + path;
        return r;
    }

    SF_INFO info{};
    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        // libsndfile can't handle this container (e.g. AAC / Apple Lossless in
        // .m4a/.mp4/.aac). Fall back to the platform codec stack.
        DecodeResult fallback;
        if (decodeCompressed(path, fallback)) return fallback;
        r.error = std::string("could not open file: ") + sf_strerror(nullptr);
        return r;
    }

    SubtypeInfo st = subtypeInfo(info.format);

    r.meta.path = path;
    r.meta.filename = fs::path(path).filename().string();
    r.meta.container = majorFormatName(info.format);
    r.meta.codec = st.codec;
    r.meta.bitDepth = st.bitDepth;
    r.meta.lossless = st.lossless;
    r.meta.sampleRate = info.samplerate;
    r.meta.channels = info.channels;
    r.meta.frames = static_cast<std::uint64_t>(info.frames);
    r.meta.durationSec = info.samplerate > 0
                             ? static_cast<double>(info.frames) / info.samplerate
                             : 0.0;
    r.meta.fileBytes = static_cast<std::uint64_t>(fs::file_size(path, ec));

    if (info.channels <= 0 || info.samplerate <= 0) {
        sf_close(snd);
        r.error = "File has no decodable audio (channels/samplerate invalid)";
        return r;
    }

    AudioBuffer& buf = r.buffer;
    buf.sampleRate = info.samplerate;
    buf.channels = info.channels;
    buf.data.assign(info.channels, std::vector<float>());

    const sf_count_t kBlock = 1 << 16;  // frames per read
    std::vector<float> inter(static_cast<std::size_t>(kBlock) * info.channels);

    // Pre-reserve when frame count is known to avoid reallocations on long files.
    if (info.frames > 0) {
        for (auto& ch : buf.data) ch.reserve(static_cast<std::size_t>(info.frames));
    }

    sf_count_t got = 0;
    while ((got = sf_readf_float(snd, inter.data(), kBlock)) > 0) {
        for (sf_count_t f = 0; f < got; ++f) {
            for (int c = 0; c < info.channels; ++c) {
                buf.data[c].push_back(inter[f * info.channels + c]);
            }
        }
    }
    sf_close(snd);

    // Trust the decoded frame count over the header (catches truncation).
    r.meta.frames = static_cast<std::uint64_t>(buf.frames());
    r.meta.durationSec = buf.durationSec();

    if (buf.frames() == 0) {
        r.error = "Decoded zero audio frames";
        return r;
    }

    r.ok = true;
    return r;
}

}  // namespace argus
