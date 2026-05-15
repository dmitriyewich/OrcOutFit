#pragma once

#include "orc_weapon_audio_types.h"

#include <vector>

class OrcIniDocument;
struct OrcIniValue;
struct OrcWeaponAudioStemContext;

/// Вызывать после применения `g_mainIniDoc` в `LoadConfig`.
void OrcWeaponAudioConfigApplyFromMainIni(const OrcIniDocument& ini);

void OrcWeaponAudioConfigAppendMainIniValues(std::vector<OrcIniValue>& values);

/// Сброс кеша sidecar (при `OrcWeaponAudioInvalidateCaches`).
void OrcWeaponAudioConfigClearStemOverrides();

OrcWeaponAudioAttenuation OrcWeaponAudioConfigBuiltinAttenuation(OrcWeaponSoundClass cls);

/// С merge: sidecar (.audio) > [WeaponAudio] INI > builtin.
OrcWeaponAudioAttenuation OrcWeaponAudioConfigResolveAttenuation(const OrcWeaponAudioStemContext* ctx, OrcWeaponSoundClass cls);

bool OrcWeaponAudioConfigEfxReverbEnabled();
bool OrcWeaponAudioConfigEfxInteriorOnly();

/// Построить play params: gainScale, spatial, класс; pitch из игры внутри openal.
OrcWeaponAudioPlayParams OrcWeaponAudioBuildPlayParams(const OrcWeaponAudioStemContext* ctx, float gainScale,
    OrcWeaponSpatial spatial, OrcWeaponSoundClass cls);

OrcWeaponSoundClass OrcWeaponInferSoundClassFromSuffix(const char* sfxSuffix);
