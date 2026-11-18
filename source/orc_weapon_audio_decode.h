#pragma once

#include <string>
#include <vector>

/// Декодирование в float PCM, затем downmix в mono для OpenAL 3D.
struct OrcAudioPcm {
    std::vector<float> samples;
    unsigned channels = 0;
    unsigned sampleRate = 0;
};

/// Поддержка: .wav .mp3 .flac .ogg (регистр расширения по пути).
bool OrcAudioDecodeFile(const char* path, OrcAudioPcm& out, std::string* errorOut = nullptr);
