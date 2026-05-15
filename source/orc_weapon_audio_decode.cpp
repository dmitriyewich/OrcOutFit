// WAV/MP3/FLAC/OGG → mono float PCM для OpenAL Soft.

#include "orc_weapon_audio_decode.h"

#include "orc_log.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#define DR_WAV_IMPLEMENTATION
#include "external/dr_libs/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "external/dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "external/dr_libs/dr_flac.h"

extern "C" {
#include "external/stb_vorbis/stb_vorbis.c"
}

static bool OrcStrEndsWithIgnoreCase(const char* path, const char* extWithDot) {
    if (!path || !extWithDot)
        return false;
    const size_t lp = strlen(path);
    const size_t le = strlen(extWithDot);
    if (lp < le)
        return false;
    for (size_t i = 0; i < le; ++i) {
        const char a = (char)tolower((unsigned char)path[lp - le + i]);
        const char b = (char)tolower((unsigned char)extWithDot[i]);
        if (a != b)
            return false;
    }
    return true;
}

static void OrcDownmixToMono(OrcAudioPcm& io) {
    if (io.channels <= 1 || io.samples.empty())
        return;
    const size_t frames = io.samples.size() / io.channels;
    std::vector<float> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
        double acc = 0.0;
        for (unsigned c = 0; c < io.channels; ++c)
            acc += (double)io.samples[f * io.channels + c];
        mono[f] = (float)(acc / (double)io.channels);
    }
    io.samples = std::move(mono);
    io.channels = 1;
}

static bool OrcDecodeWav(const char* path, OrcAudioPcm& out, std::string* err) {
    drwav wav{};
    if (!drwav_init_file(&wav, path, nullptr)) {
        if (err)
            *err = "drwav_init_file failed";
        return false;
    }
    drwav_uint64 frames = wav.totalPCMFrameCount;
    if (frames == 0 || wav.channels == 0) {
        drwav_uninit(&wav);
        if (err)
            *err = "empty wav";
        return false;
    }
    std::vector<float> buf(static_cast<size_t>(frames) * wav.channels);
    drwav_uint64 n = drwav_read_pcm_frames_f32(&wav, frames, buf.data());
    drwav_uninit(&wav);
    if (n == 0) {
        if (err)
            *err = "drwav read 0 frames";
        return false;
    }
    buf.resize(static_cast<size_t>(n) * wav.channels);
    out.samples = std::move(buf);
    out.channels = wav.channels;
    out.sampleRate = wav.sampleRate;
    return true;
}

static bool OrcDecodeMp3(const char* path, OrcAudioPcm& out, std::string* err) {
    drmp3_config cfg{};
    drmp3_uint64 total = 0;
    float* p = drmp3_open_file_and_read_pcm_frames_f32(path, &cfg, &total, nullptr);
    if (!p || total == 0 || cfg.channels == 0) {
        if (p)
            drmp3_free(p, nullptr);
        if (err)
            *err = "drmp3 failed";
        return false;
    }
    out.samples.assign(p, p + static_cast<size_t>(total) * cfg.channels);
    drmp3_free(p, nullptr);
    out.channels = cfg.channels;
    out.sampleRate = cfg.sampleRate;
    return true;
}

static bool OrcDecodeFlac(const char* path, OrcAudioPcm& out, std::string* err) {
    unsigned ch = 0, sr = 0;
    drflac_uint64 frames = 0;
    float* p = drflac_open_file_and_read_pcm_frames_f32(path, &ch, &sr, &frames, nullptr);
    if (!p || frames == 0 || ch == 0) {
        if (p)
            drflac_free(p, nullptr);
        if (err)
            *err = "drflac failed";
        return false;
    }
    out.samples.assign(p, p + static_cast<size_t>(frames) * ch);
    drflac_free(p, nullptr);
    out.channels = ch;
    out.sampleRate = sr;
    return true;
}

static bool OrcDecodeOgg(const char* path, OrcAudioPcm& out, std::string* err) {
    short* decoded = nullptr;
    int ch = 0, sr = 0;
    int total = stb_vorbis_decode_filename(path, &ch, &sr, &decoded);
    if (total <= 0 || !decoded || ch <= 0) {
        if (decoded)
            free(decoded);
        if (err)
            *err = "stb_vorbis failed";
        return false;
    }
    const int samples = total;
    out.samples.resize(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        float v = static_cast<float>(decoded[i]) / 32768.0f;
        out.samples[static_cast<size_t>(i)] = std::clamp(v, -1.0f, 1.0f);
    }
    free(decoded);
    out.channels = static_cast<unsigned>(ch);
    out.sampleRate = static_cast<unsigned>(sr);
    return true;
}

bool OrcAudioDecodeFile(const char* pathUtf8, OrcAudioPcm& out, std::string* errorOut) {
    out = {};
    if (!pathUtf8 || !pathUtf8[0]) {
        if (errorOut)
            *errorOut = "empty path";
        return false;
    }
    std::string err;
    bool ok = false;
    if (OrcStrEndsWithIgnoreCase(pathUtf8, ".wav"))
        ok = OrcDecodeWav(pathUtf8, out, &err);
    else if (OrcStrEndsWithIgnoreCase(pathUtf8, ".mp3"))
        ok = OrcDecodeMp3(pathUtf8, out, &err);
    else if (OrcStrEndsWithIgnoreCase(pathUtf8, ".flac"))
        ok = OrcDecodeFlac(pathUtf8, out, &err);
    else if (OrcStrEndsWithIgnoreCase(pathUtf8, ".ogg"))
        ok = OrcDecodeOgg(pathUtf8, out, &err);
    else {
        if (errorOut)
            *errorOut = "unknown extension";
        return false;
    }
    if (!ok) {
        if (errorOut)
            *errorOut = err;
        return false;
    }
    if (out.channels > 1 && g_orcLogLevel >= OrcLogLevel::Info)
        OrcLogInfo("weapon audio decode: stereo/multichannel -> mono (%s)", pathUtf8);
    OrcDownmixToMono(out);
    return !out.samples.empty() && out.sampleRate > 0 && out.channels == 1;
}
