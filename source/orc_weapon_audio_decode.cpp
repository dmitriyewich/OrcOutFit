// WAV/MP3/FLAC/OGG → mono float PCM для OpenAL Soft.

#include "orc_weapon_audio_decode.h"

#include "orc_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>

#include <windows.h>

#define DR_WAV_IMPLEMENTATION
#include "external/dr_libs/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "external/dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "external/dr_libs/dr_flac.h"

extern "C" {
#include "external/stb_vorbis/stb_vorbis.c"
}

namespace {
constexpr size_t kMaxDecodedFrames = 4 * 1024 * 1024; // 16 MiB моно PCM
constexpr DWORD kMaxEncodedBytes = 64 * 1024 * 1024;
constexpr unsigned kMaxSampleRate = 384000;
constexpr unsigned kMaxChannels = 64;

template <typename ReadFrames>
bool OrcDecodeMono(unsigned channels, unsigned rate, uint64_t expectedFrames, ReadFrames read,
    OrcAudioPcm& out, const char*& error) {
    if (channels == 0 || channels > kMaxChannels || rate == 0 || rate > kMaxSampleRate) {
        error = "invalid channels or sample rate";
        return false;
    }
    if (expectedFrames > kMaxDecodedFrames) {
        error = "decoded audio exceeds 16 MiB";
        return false;
    }

    OrcAudioPcm pcm;
    pcm.samples.reserve(static_cast<size_t>(expectedFrames));
    std::array<float, 4096> interleaved;
    const double channelScale = 1.0 / channels;
    for (;;) {
        const size_t offset = pcm.samples.size();
        const size_t remaining = kMaxDecodedFrames - offset;
        const size_t requested = std::min(interleaved.size() / channels, remaining + 1);
        const uint64_t framesRead = read(interleaved.data(), requested);
        if (framesRead > remaining) {
            error = "decoded audio exceeds 16 MiB";
            return false;
        }
        const size_t frames = static_cast<size_t>(framesRead);
        pcm.samples.resize(offset + frames);
        for (size_t frame = 0; frame < frames; ++frame) {
            double sum = 0.0;
            const float* samples = interleaved.data() + frame * channels;
            for (unsigned c = 0; c < channels; ++c) {
                if (std::isfinite(samples[c]))
                    sum += samples[c];
            }
            pcm.samples[offset + frame] = std::clamp(static_cast<float>(sum * channelScale), -1.0f, 1.0f);
        }
        if (frames < requested)
            break;
    }
    if (pcm.samples.empty() || (expectedFrames != 0 && pcm.samples.size() != expectedFrames)) {
        error = "empty or truncated audio";
        return false;
    }

    pcm.sampleRate = rate;
    pcm.channels = 1;
    out = std::move(pcm);
    return true;
}

bool OrcDecodeWav(const char* path, OrcAudioPcm& out, const char*& error) {
    drwav wav{};
    if (!drwav_init_file(&wav, path, nullptr)) {
        error = "invalid WAV";
        return false;
    }
    const std::unique_ptr<drwav, decltype(&drwav_uninit)> close(&wav, drwav_uninit);
    return OrcDecodeMono(wav.channels, wav.sampleRate, wav.totalPCMFrameCount,
        [&](float* samples, size_t frames) { return drwav_read_pcm_frames_f32(&wav, frames, samples); }, out, error);
}

bool OrcDecodeMp3(const char* path, OrcAudioPcm& out, const char*& error) {
    drmp3 mp3{};
    if (!drmp3_init_file(&mp3, path, nullptr)) {
        error = "invalid MP3";
        return false;
    }
    const std::unique_ptr<drmp3, decltype(&drmp3_uninit)> close(&mp3, drmp3_uninit);
    return OrcDecodeMono(mp3.channels, mp3.sampleRate, 0,
        [&](float* samples, size_t frames) { return drmp3_read_pcm_frames_f32(&mp3, frames, samples); }, out, error);
}

bool OrcDecodeFlac(const char* path, OrcAudioPcm& out, const char*& error) {
    const std::unique_ptr<drflac, decltype(&drflac_close)> flac(drflac_open_file(path, nullptr), drflac_close);
    if (!flac) {
        error = "invalid FLAC";
        return false;
    }
    return OrcDecodeMono(flac->channels, flac->sampleRate, flac->totalPCMFrameCount,
        [&](float* samples, size_t frames) { return drflac_read_pcm_frames_f32(flac.get(), frames, samples); }, out, error);
}

bool OrcDecodeOgg(const char* path, OrcAudioPcm& out, const char*& error) {
    int vorbisError = 0;
    const std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> ogg(
        stb_vorbis_open_filename(path, &vorbisError, nullptr), stb_vorbis_close);
    if (!ogg) {
        error = "invalid Ogg Vorbis";
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(ogg.get());
    const unsigned expectedFrames = stb_vorbis_stream_length_in_samples(ogg.get());
    return OrcDecodeMono(info.channels, info.sample_rate, expectedFrames,
        [&](float* samples, size_t frames) {
            return stb_vorbis_get_samples_float_interleaved(
                ogg.get(), info.channels, samples, static_cast<int>(frames * info.channels));
        }, out, error);
}
} // namespace

bool OrcAudioDecodeFile(const char* path, OrcAudioPcm& out, std::string* errorOut) {
    out = {};
    const char* error = "";
    bool ok = false;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!path || !path[0]) {
        error = "empty path";
    } else if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "audio file unavailable";
    } else if (attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow > kMaxEncodedBytes) {
        error = "audio file exceeds 64 MiB";
    } else {
        const char* ext = std::strrchr(path, '.');
        if (ext && _stricmp(ext, ".wav") == 0)
            ok = OrcDecodeWav(path, out, error);
        else if (ext && _stricmp(ext, ".mp3") == 0)
            ok = OrcDecodeMp3(path, out, error);
        else if (ext && _stricmp(ext, ".flac") == 0)
            ok = OrcDecodeFlac(path, out, error);
        else if (ext && _stricmp(ext, ".ogg") == 0)
            ok = OrcDecodeOgg(path, out, error);
        else
            error = "unknown extension";
    }
    if (errorOut)
        *errorOut = error;
    return ok;
}