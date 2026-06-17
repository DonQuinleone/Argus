// macOS AAC / Apple Lossless decode via CoreAudio's ExtAudioFile (C API in the
// AudioToolbox framework — no Objective-C, so it links cleanly into the headless
// core used by the CLI). Converts to deinterleaved float32 at the source rate.
#include "PlatformDecode.h"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace argus {

bool decodeCompressed(const std::string& path, DecodeResult& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;

    CFStringRef cfPath =
        CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    if (!cfPath) return false;
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfPath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfPath);
    if (!url) return false;

    ExtAudioFileRef af = nullptr;
    OSStatus st = ExtAudioFileOpenURL(url, &af);
    CFRelease(url);
    if (st != noErr || !af) return false;

    AudioStreamBasicDescription src{};
    UInt32 sz = sizeof(src);
    if (ExtAudioFileGetProperty(af, kExtAudioFileProperty_FileDataFormat, &sz, &src) != noErr) {
        ExtAudioFileDispose(af);
        return false;
    }

    const int channels = src.mChannelsPerFrame > 0 ? static_cast<int>(src.mChannelsPerFrame) : 2;
    const double sampleRate = src.mSampleRate > 0 ? src.mSampleRate : 44100.0;

    // Ask CoreAudio to convert to interleaved float32 at the source rate.
    AudioStreamBasicDescription cli{};
    cli.mSampleRate = sampleRate;
    cli.mFormatID = kAudioFormatLinearPCM;
    cli.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cli.mBitsPerChannel = 32;
    cli.mChannelsPerFrame = static_cast<UInt32>(channels);
    cli.mFramesPerPacket = 1;
    cli.mBytesPerFrame = static_cast<UInt32>(channels) * sizeof(float);
    cli.mBytesPerPacket = cli.mBytesPerFrame;
    if (ExtAudioFileSetProperty(af, kExtAudioFileProperty_ClientDataFormat, sizeof(cli), &cli) !=
        noErr) {
        ExtAudioFileDispose(af);
        return false;
    }

    AudioBuffer& buf = out.buffer;
    buf.sampleRate = static_cast<int>(sampleRate);
    buf.channels = channels;
    buf.data.assign(channels, std::vector<float>());

    const UInt32 kFrames = 1 << 14;
    std::vector<float> inter(static_cast<std::size_t>(kFrames) * channels);
    for (;;) {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = static_cast<UInt32>(channels);
        abl.mBuffers[0].mDataByteSize = kFrames * channels * sizeof(float);
        abl.mBuffers[0].mData = inter.data();
        UInt32 n = kFrames;
        if (ExtAudioFileRead(af, &n, &abl) != noErr) {
            ExtAudioFileDispose(af);
            return false;
        }
        if (n == 0) break;
        for (UInt32 f = 0; f < n; ++f)
            for (int c = 0; c < channels; ++c)
                buf.data[c].push_back(inter[f * channels + c]);
    }
    ExtAudioFileDispose(af);

    if (buf.frames() == 0) return false;

    const UInt32 fid = src.mFormatID;
    const bool alac = fid == kAudioFormatAppleLossless;
    out.meta.path = path;
    out.meta.filename = fs::path(path).filename().string();
    std::string ext = fs::path(path).extension().string();
    out.meta.container = (ext == ".aac") ? "ADTS/AAC" : "MPEG-4";
    out.meta.codec = alac ? "Apple Lossless (ALAC)"
                          : (fid == kAudioFormatMPEG4AAC ? "AAC (MPEG-4)" : "compressed");
    out.meta.bitDepth = alac ? (src.mBitsPerChannel ? static_cast<int>(src.mBitsPerChannel) : 16) : 0;
    out.meta.lossless = alac;
    out.meta.sampleRate = static_cast<int>(sampleRate);
    out.meta.channels = channels;
    out.meta.frames = static_cast<std::uint64_t>(buf.frames());
    out.meta.durationSec = buf.durationSec();
    out.meta.fileBytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
    out.ok = true;
    return true;
}

}  // namespace argus
#endif  // __APPLE__
