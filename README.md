# OrcOutFit

<img width="238" height="408" alt="kHKmcxq" src="https://github.com/user-attachments/assets/44e51b41-9600-4cb8-ae4e-611afc44ed08" />

Нативный **ASI-плагин** для **GTA San Andreas 1.0 US** (x86), включая **SA:MP**.  
Полная документация и инструкции перенесены в wiki: [OrcOutFit Wiki](https://github.com/dmitriyewich/OrcOutFit/wiki).

---

## Возможности

- Отображение оружия на теле педа с настройкой позиции, поворота и масштаба.
- Коррекция позы оружия в руках (Held) через пресет `OrcOutFit\Weapons\<dff>.ini` (см. wiki «Оружие»); точка спавна FX выстрела (`muzzlePosn`: дым, гильзы и др.) считается в той же **Held**-трансформации, что и видимый меш (предпочтительно через LTM кадра оружия в клумпе и базовую позу движка для кадра; иначе — через кость **R_Hand** + `m_vecFireOffset` из `weapon.dat` / `CWeaponInfo`); **направление пули** (`origin`) — по ваниле. Вспышка у ствола (`gunflash` в DFF): при замене модели в руках узел синхронизируется с клумпом видимого меша (клон замены), а не с параллельным стоком в слоте, если слот временно указывает на сток; на атомики под dummy `gunflash` **не** крутится тот же `OrcTryApplyHeldPoseOneFrame`, что на меш (локальные LTMs другие — ломает DoGunFlash); вместо этого кадр `gunflash` **сдвигается на мировую дельту** точки `m_vecFireOffset` (Held − ванильная кость R_Hand), как у `muzzlePosn`. На **теле** меш вспышки на кобуре не рисуется постоянно (см. wiki).
- Замена моделей и текстур оружия (включая сценарии для SA:MP); папка оружия берётся по DFF-имени из `LoadWeaponObject` / IDE (`desert_eagle`, `chromegun`, `mp5lng` и т.п.), random-пулы DFF/TXD могут выбираться случайно, последовательно по кругу или случайно без повтора среди живых ped. Ручной рескан замены перечитывает `Guns` / `GunsNick` и заново выбирает random-варианты для уже известных ped. При **PRO** и twin-пистолетах (`colt45` и аналоги) замена в руках работает для **обеих** рук — см. wiki «Оружие» → «Замена оружия».
- Опционально: кастомные звуки для заменённого оружия — **.wav / .mp3 / .flac / .ogg** (моно после декода; стерео сводится в mono), настройка 3D-затухания и **EFX reverb** в `[WeaponAudio]` и опционально в `<stem>.audio` рядом с DFF (вкладка **Оружие → Звуки**, `[Features] CustomWeaponSounds=1` — см. wiki «Оружие»); локальный ped звучит как раньше, остальные ped — как 3D-источники с отсечением по `MaxDist`; воспроизведение через **OpenAL Soft** внутри `OrcOutFit.asi`.
- Кастомные и стандартные объекты с привязкой к скинам.
- Кастомные, стандартные и random-скины; random-пулы поддерживают случайный, последовательный и случайный без повтора режим выбора, а `SkinRandomIncludeVanilla=1` добавляет исходный ped как отдельный вариант. Кастомный DFF и выбранная/привязанная стандартная ped-модель по умолчанию применяются как native replacement: штатный `CPed::SetModelIndex` при создании/стриминге ped перехватывается до создания clump, а gameplay model ID остаётся базовым. Overlay оставлен как legacy-режим. Для non-local/random ped поздний frame update не вызывает повторный `SetModelIndex`, поэтому пропущенная первичная замена ждёт естественного респавна/смены модели. Random-пулы стандартных ped привязаны к DFF-имени из `LoadPedObject` (`TRUTH`, `BMYDRUG`, `lapd1` и т.п.).
- Texture remap для стандартных ped-текстур.
- Настройка через ImGui-меню и INI-файлы.

Подробнее по вкладкам и подвкладкам:
- [Home (описание проекта)](https://github.com/dmitriyewich/OrcOutFit/wiki)
- [Главная](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%93%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F)
- [Оружие](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%9E%D1%80%D1%83%D0%B6%D0%B8%D0%B5)
- [Объекты](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%9E%D0%B1%D1%8A%D0%B5%D0%BA%D1%82%D1%8B)
- [Скины](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%A1%D0%BA%D0%B8%D0%BD%D1%8B)
- [Настройки](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%9D%D0%B0%D1%81%D1%82%D1%80%D0%BE%D0%B9%D0%BA%D0%B8)

---

## Требования и ограничения

| Параметр | Значение |
|----------|----------|
| Игра | GTA San Andreas **1.0 US** |
| Архитектура | **Win32 (x86)** |
| SA:MP | Поддерживаемые `samp.dll`: **R1, R2, R3, R3-1, R4, R4-2, R5-1, DL-R1** |
| Ограничение | Для клиентов вне списка SA:MP-часть может работать частично или в fallback-режиме |
| Ограничение | Не заменяйте ASI-файл во время работы игры |

---

## Установка

1. Получите `OrcOutFit.asi` — готовый релиз или локальная сборка.
2. Поместите `OrcOutFit.asi` в каталог игры (или в modloader).
3. Запустите игру. Рядом с ASI появится или будет прочитан `OrcOutFit.ini`.
4. Откройте меню клавишей `F7` в одиночной игре или командой `/orcoutfit` в SA:MP. Для `F7` в SA:MP включите `[Main] SampAllowActivationKey=1`.
5. При необходимости задайте уровень лога в `[Features] DebugLogLevel`: `0` — в `OrcOutFit.log` ничего не пишется (включая ошибки), `1` — только ошибки, `2` — полный trace. При каждой новой игровой сессии лог рядом с INI пересоздаётся; при уровне `0` он остаётся пустым.

При переходе с Lite уберите `OrcOutFitLite.asi` из каталога игры или ModLoader. Существующий `OrcOutFit.ini` можно использовать с `OrcOutFit.asi`.

## Сборка из исходников

Нужны Visual Studio 18 с инструментами C++ x86 (`v145`) и CMake. Выполните из корня репозитория в PowerShell.

```powershell
git submodule update --init --recursive
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$env:PLUGIN_SDK_DIR = (Resolve-Path 'source/external/plugin-sdk').Path
& $msbuild source/external/plugin-sdk/plugin_sa/Plugin_SA.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
& $msbuild OrcOutFit.sln /t:Build /p:Configuration=Release /p:Platform=x86
```

PreBuild собирает статический OpenAL Soft 1.24.3. Плагин загружается без отдельной `OpenAL32.dll`. Артефакт — `build/Release/OrcOutFit.asi`. Локальная сборка также создаёт PDB. GitHub Actions использует `v143` и собирает ASI без PDB.

---

## Автор

**[@dmitriyewich](https://github.com/dmitriyewich)** — [OrcOutFit](https://github.com/dmitriyewich/OrcOutFit)
