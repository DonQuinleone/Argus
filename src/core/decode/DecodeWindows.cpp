// Windows AAC / Apple Lossless (.m4a/.mp4/.aac) decode via Media Foundation's
// IMFSourceReader, negotiated to interleaved float32 PCM. Linked against
// mfplat/mfreadwrite/mfuuid/ole32. Compiled only on Windows.
#include "PlatformDecode.h"

#if defined(_WIN32)
#include <windows.h>
// clang-format off
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <Mfobjects.h>
// clang-format on

#include <filesystem>
#include <string>
#include <vector>

namespace argus {
namespace {

template <class T>
void safeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

}  // namespace

bool decodeCompressed(const std::string& path, DecodeResult& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;

    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (comInit) CoUninitialize();
        return false;
    }

    bool ok = false;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* partialType = nullptr;
    IMFMediaType* actualType = nullptr;

    do {
        std::wstring wpath = widen(path);
        if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader))) break;

        // Decode all streams except the first audio stream off; select audio.
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

        // Request interleaved float32 PCM output.
        if (FAILED(MFCreateMediaType(&partialType))) break;
        partialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        partialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                               partialType)))
            break;

        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actualType)))
            break;

        UINT32 channels = 0, sampleRate = 0, bitsPerSample = 0;
        actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
        actualType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
        if (channels == 0 || sampleRate == 0 || bitsPerSample != 32) break;

        AudioBuffer& buf = out.buffer;
        buf.sampleRate = static_cast<int>(sampleRate);
        buf.channels = static_cast<int>(channels);
        buf.data.assign(channels, std::vector<float>());

        for (;;) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                                            nullptr, &sample);
            if (FAILED(hr)) { safeRelease(sample); break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { safeRelease(sample); ok = true; break; }
            if (!sample) continue;  // gap / format change without data

            IMFMediaBuffer* mbuf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mbuf)) && mbuf) {
                BYTE* data = nullptr;
                DWORD cur = 0;
                if (SUCCEEDED(mbuf->Lock(&data, nullptr, &cur))) {
                    const float* f = reinterpret_cast<const float*>(data);
                    std::size_t frames = cur / (sizeof(float) * channels);
                    for (std::size_t fr = 0; fr < frames; ++fr)
                        for (UINT32 c = 0; c < channels; ++c)
                            buf.data[c].push_back(f[fr * channels + c]);
                    mbuf->Unlock();
                }
            }
            safeRelease(mbuf);
            safeRelease(sample);
        }

        if (ok && buf.frames() > 0) {
            std::string ext = fs::path(path).extension().string();
            out.meta.path = path;
            out.meta.filename = fs::path(path).filename().string();
            out.meta.container = (ext == ".aac") ? "ADTS/AAC" : "MPEG-4";
            out.meta.codec = "AAC / ALAC (Media Foundation)";
            out.meta.bitDepth = 0;
            out.meta.lossless = false;
            out.meta.sampleRate = static_cast<int>(sampleRate);
            out.meta.channels = static_cast<int>(channels);
            out.meta.frames = static_cast<std::uint64_t>(buf.frames());
            out.meta.durationSec = buf.durationSec();
            out.meta.fileBytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
            out.ok = true;
        } else {
            ok = false;
        }
    } while (false);

    safeRelease(actualType);
    safeRelease(partialType);
    safeRelease(reader);
    MFShutdown();
    if (comInit) CoUninitialize();
    return ok && out.ok;
}

}  // namespace argus
#endif  // _WIN32
