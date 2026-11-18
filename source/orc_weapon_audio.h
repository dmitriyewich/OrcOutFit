#pragma once

/// Контекст OpenAL + слушатель; пауза при меню.
void OrcWeaponAudioShutdown();

/// Установка хуков после включения звуков и обработка источников в главном потоке.
void OrcWeaponAudioOnGameProcess();

/// Остановить источники при перезапуске мира или приостановке плагина.
void OrcWeaponAudioStopPlayback();

/// Сброс путей, декодированных буферов и настроек .audio после пересканирования.
void OrcWeaponAudioInvalidateCaches();
