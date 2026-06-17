// Linux (and any non-Apple/Windows host) AAC / Apple Lossless decode via FFmpeg's
// libavformat/libavcodec. Compiled only when CMake finds FFmpeg (ARGUS_HAVE_FFMPEG).
// Sample formats are converted to float by hand so we don't depend on the
// libswresample channel-layout API, which changed across FFmpeg major versions.
#include "PlatformDecode.h"

#if defined(ARGUS_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/version.h>
}

#include <cstdint>
#include <filesystem>
#include <vector>

namespace argus {
namespace {

// One sample (channel `ch`, frame index `i`) from an AVFrame, normalised to float.
float sampleAt(const AVFrame* fr, int fmt, int ch, int i, int nch) {
    const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(fmt)) != 0;
    const uint8_t* base = planar ? fr->data[ch] : fr->data[0];
    const int idx = planar ? i : (i * nch + ch);
    switch (fmt) {
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            return reinterpret_cast<const float*>(base)[idx];
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
            return static_cast<float>(reinterpret_cast<const double*>(base)[idx]);
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            return reinterpret_cast<const int16_t*>(base)[idx] / 32768.0f;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            return static_cast<float>(reinterpret_cast<const int32_t*>(base)[idx] / 2147483648.0);
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
            return (static_cast<int>(base[idx]) - 128) / 128.0f;
        default:
            return 0.0f;
    }
}

int frameChannels(const AVCodecContext* ctx) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
    return ctx->ch_layout.nb_channels;
#else
    return ctx->channels;
#endif
}

}  // namespace

bool decodeCompressed(const std::string& path, DecodeResult& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecParameters* par = fmt->streams[stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        if (ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    const int channels = frameChannels(ctx) > 0 ? frameChannels(ctx) : par->ch_layout.nb_channels;
    const int sampleRate = ctx->sample_rate > 0 ? ctx->sample_rate : par->sample_rate;
    if (channels <= 0 || sampleRate <= 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AudioBuffer& buf = out.buffer;
    buf.sampleRate = sampleRate;
    buf.channels = channels;
    buf.data.assign(channels, std::vector<float>());

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = true;
    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            const int n = frame->nb_samples;
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < channels; ++c)
                    buf.data[c].push_back(sampleAt(frame, frame->format, c, i, channels));
        }
    };
    while (av_read_frame(fmt, pkt) == 0) {
        if (pkt->stream_index == stream) {
            if (avcodec_send_packet(ctx, pkt) == 0) drain();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);  // flush
    drain();

    av_frame_free(&frame);
    av_packet_free(&pkt);

    const bool alac = par->codec_id == AV_CODEC_ID_ALAC;
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    if (!ok || buf.frames() == 0) return false;

    std::string ext = fs::path(path).extension().string();
    out.meta.path = path;
    out.meta.filename = fs::path(path).filename().string();
    out.meta.container = (ext == ".aac") ? "ADTS/AAC" : "MPEG-4";
    out.meta.codec = alac ? "Apple Lossless (ALAC)" : "AAC (MPEG-4)";
    out.meta.bitDepth = 0;
    out.meta.lossless = alac;
    out.meta.sampleRate = sampleRate;
    out.meta.channels = channels;
    out.meta.frames = static_cast<std::uint64_t>(buf.frames());
    out.meta.durationSec = buf.durationSec();
    out.meta.fileBytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
    out.ok = true;
    return true;
}

}  // namespace argus
#endif  // ARGUS_HAVE_FFMPEG
