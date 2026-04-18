# Context

## Правила работы (обязательно)

- После каждого заметного изменения кода/логики/сборки обязательно обновлять `.claude/work.md` до ответа пользователю.
- Без явной команды пользователя не делать `git push` (исключение: пользователь прямо просит запушить).
- Идентификация стандартного ped для INI/пресетов: **имя DFF из хука `LoadPedObject`** (`modelId → имя` из `ped.dat`). Отдельные пользовательские пути вида `id###` для папок пресетов **не используются** (см. `OrcOutFit\Weapons\<dff>.ini` и `[Skin.<dff>]`).
- Для оружия использовать `weapon.dat`-first подход (хук `LoadWeaponObject`) с безопасным fallback.
- После существенных правок обязательно делать проверочную сборку `Release|Win32`.
- Документацию (`context.md`, `README.md`, `README.txt`) синхронизировать с текущей реализацией перед финальным коммитом по запросу пользователя.

## Назначение проекта

- `OrcOutFit` это нативный `ASI`-плагин для `GTA San Andreas / SA:MP`.
- Публичный репозиторий: `https://github.com/dmitriyewich/OrcOutFit`
- Плагин рендерит:
  - оружие на теле ped (локальный игрок + опционально все ped),
  - кастомные объекты из папки `Objects`,
  - кастомный skin-режим для локального игрока из папки `Skins`.
- Оружие, которое сейчас в руках у ped, на теле не рисуется.
- Выходной артефакт проекта:
  - `OrcOutFit.asi`

## Целевая платформа

- GTA San Andreas `1.0 US`.
- Только `Win32` (x86).

## Текущая рабочая логика

- Главный рендер идёт из `Events::drawingEvent`.
- Оружие локального игрока:
  - синхронизация слотов `m_aWeapons`,
  - `CreateInstance(RwMatrix*)` для нужных моделей,
  - рендер на кости через `RpHAnim` + `RpClumpRender`/atomic callback.
- Авто-скан оружия (modded weapon.dat friendly):
  - основной источник — хук `CFileLoader::LoadWeaponObject` (`0x5B3FB0`, MinHook), который заполняет кеш
    `wt -> modelId` по реально загруженным строкам `weapon.dat`;
  - fallback/дополнение — чтение `aWeaponInfo`/`GetWeaponInfo` (SEH-safe) для совместимости;
  - UI показывает оружие в формате `Name [weaponTypeId][modelId]`, а также поддерживает полный диапазон `0..256` для отладки;
  - если при рендере оружия его модель ещё не загружена, плагин автоматически запрашивает стриминг
    (`CStreaming::RequestModel` + `LoadAllRequestedModels`) и начинает рендерить со следующего кадра.
- Dual wield (weapon skills):
  - опция `Main.ConsiderWeaponSkills`,
  - если у оружия `CWeaponInfo::m_nFlags.bTwinPistol` (проверка skill-info: `GetWeaponInfo(wt,2)` с fallback на `1`)
    и `ped->GetWeaponSkill(wt) == WEAPSKILL_PRO` —
    рендерится второй инстанс оружия,
  - проверка навыка делается для конкретного ped (локальный и все ped в режиме `RenderAllPedsWeapons`),
  - настройки второго ствола хранятся отдельно (`WeaponNN_2` / `[Pistol2]`, etc.).
- Оружие всех ped (опция):
  - включается флагом `RenderAllPedsWeapons`,
  - фильтруется по радиусу `RenderAllPedsRadius`,
  - используется per-ped cache инстансов по `CPools::GetPedRef`.
- UI:
  - вкладки **Main** (плагин, `[Features]`, пути), **Weapons**, **Objects**, **Skins**;
  - список стандартных ped из кеша `LoadPedObject`: **сортировка по `model id` по возрастанию**, подписи **`Имя [ID]`** (Weapons и Objects);
  - **Wear this skin**: очередь смены модели локального педа — в начале `drawingEvent` выполняются `CStreaming::RequestModel` / `LoadAllRequestedModels`, проверка `MODEL_INFO_PED`, `ClearAll()` по прикреплённому оружию, затем `CPed::SetModelIndex` @ `0x5E4880`;
  - **Save to `OrcOutFit\Weapons`**: блокируется только в **single-player** для **дефолтного CJ** (`MODEL_PLAYER` + `PED_TYPE_PLAYER1` + нет `samp.dll`); иначе доступно (в т.ч. SA:MP и после примерки скина).
- UI оружия:
  - рядом с `Show on body` есть `Copy`/`Paste` для текущего `WeaponCfg`,
  - Paste валидирует буфер и может вставлять между primary/secondary (dual wield).
- Кастомные объекты:
  - скан `*.dff` в `OrcOutFit\Objects`,
  - отдельный `<name>.ini` на каждый объект; для каждого стандартного скина ped — секция `[Skin.<dff_name>]` (имя из `LoadPedObject`, без `object\other`).
  - масштаб: общий `Scale` и дополнительные множители `ScaleX/ScaleY/ScaleZ` (по осям).
  - объект может быть условным по оружию (внутри секции `[Skin.*]`):
    - `Weapons=` (csv weapon ids),
    - `WeaponsMode=any|all`,
    - `HideWeapons=1` — при срабатывании условия скрывать выбранное оружие на теле.
- Разрешение имени модели ped:
  - хук `CFileLoader::LoadPedObject` (`0x5B7420`, MinHook) кеширует `modelId -> modelName` (dff из ped.dat);
  - для пользовательских путей/INI используется это имя; числовой fallback папок `id###` не используется.
- Skin mode:
  - скан `*.dff` в `OrcOutFit\Skins` (папка `Skins`, не `SKINS`),
  - рендер выбранного clump поверх локального ped,
  - скрытие базового ped (опция),
  - bind анимации через `RpSkinAtomicSetHAnimHierarchy`;
  - случайные пулы `Skins\random` в текущей версии **не сканируются** (ключ `RandomFromPools` в INI остаётся для совместимости).
- Per-skin weapon overrides (по имени DFF ped из ped.dat):
  - `OrcOutFit\Weapons\<skin>.ini` (регистр имени файла не важен),
  - полный набор секций оружия как в `OrcOutFit.ini`; приоритет выше глобального `OrcOutFit.ini`.

## Важные файлы

- Основной код:
  - `C:\Games\CODEX\WeaponsOutFit\source\main.cpp`
  - UI (ImGui): `source\orc_ui.cpp`, `source\orc_ui.h`; общие типы: `source\orc_types.h`; мост к состоянию: `source\orc_app.h`
- MinHook:
  - `C:\Games\CODEX\WeaponsOutFit\source\external\MinHook\`
- Проект Visual Studio:
  - `C:\Games\CODEX\WeaponsOutFit\OrcOutFit.vcxproj`
- Solution:
  - `C:\Games\CODEX\WeaponsOutFit\OrcOutFit.sln`
- Конфиг:
  - рядом с ASI: `OrcOutFit.ini`
- Этот контекст:
  - `C:\Games\CODEX\WeaponsOutFit\context.md`
- Рабочий журнал:
  - `C:\Games\CODEX\WeaponsOutFit\.claude\work.md`
- README для пользователей / форума:
  - `README.md`, `README.txt` (BB-code)

## Ключевые offsets и адреса (GTA SA 1.0 US)

- `CPed` поля:
  - `m_pRwClump` = `+0x18`
  - `m_apBones` = `+0x490`
  - `m_aWeapons` = `+0x5A0`
  - `m_nSelectedWepSlot` = `+0x718`
- `CBaseModelInfo`:
  - `m_pRwObject` = `+0x1C`
  - `m_usageCount` = `+0x1A`
  - `CreateInstance(RwMatrix*)` = `vtable[10]`
- `RpAtomic`:
  - `parentFrame` = `+0x4`
  - `renderCallBack` = `+0x44`
- `RwFrame.modelling` = `+0x10`
- Render:
  - `AtomicDefaultRenderCallBack` = `0x7491C0`
  - `RpClumpRender` = `0x749B20`
- Хук:
  - `drawingEvent` call-site = `0x53E293` (plugin-sdk `Events::drawingEvent`, `PRIORITY_AFTER`)

## Поддерживаемые кости

- Bone NODE IDs (не индексы `m_apBones`):
  - `BACK` = `3` (`BONE_SPINE1`)
  - `HIP_LEFT` = `41` (`BONE_L_THIGH`)
  - `HIP_RIGHT` = `51` (`BONE_R_THIGH`)

## Правила сборки

- Проект только `Win32`.
- Основной сценарий работы `build -> release`.
- `MSBuild` использовать только по пути:
  - `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
- Целевая конфигурация:
  - `Release | Win32` в `.vcxproj`; в `.sln` платформа называется **`x86`** (маппится на `Win32`).
  - Пример: `MSBuild.exe OrcOutFit.sln /p:Configuration=Release /p:Platform=x86`
- Ожидаемый выходной файл:
  - `C:\Games\CODEX\WeaponsOutFit\build\Release\OrcOutFit.asi`

- Важно: `Plugin.lib` (из `plugin-sdk`) должен существовать в:
  - `source\external\plugin-sdk\output\lib\Plugin.lib`
  - В репозитории он может отсутствовать (игнорируется как артефакт). Если линковка ругается на `Plugin.lib`,
    его нужно собрать из vendored `plugin-sdk` через `premake5` (см. README).
- **НЕЛЬЗЯ** автоматически копировать / перемещать `.asi` куда-либо.
  - Единственное допустимое расположение — `build\Release\OrcOutFit.asi`.
  - Деплой в папку игры (`C:\Games\SAMP\GTA San Andreas\`) делает пользователь вручную.
  - Причина: `.asi` в игре бывает залочен запущенным процессом, а двойная копия рассинхронизируется.

## Пути данных плагина (runtime)

- Путь считается относительно расположения `OrcOutFit.asi` (modloader-friendly).
- Ожидаемые подпапки:
  - `OrcOutFit\Objects`
  - `OrcOutFit\Weapons`
  - `OrcOutFit\Skins`

## Правила `reference`

- Папка `C:\Games\CODEX\WeaponsOutFit\reference` используется под:
  - `BaseModelRender-master` — референс по рендеру из `drawingEvent`
  - `CLEOPlus-main` — референс по рендеру из `pedRenderEvent.after`
  - дизассемблерные и текстовые заметки
- Эти материалы не копируются в корень проекта.

## Правила `.claude`

- В проекте должна существовать папка:
  - `C:\Games\CODEX\WeaponsOutFit\.claude`
- Мои действия и рабочий журнал сохраняются в `.claude\work.md`.
- **ПРАВИЛО**: после каждой заметной порции изменений (коммит, багфикс,
  новая фича, смена подхода) я ОБЯЗАН дописывать секцию в `work.md`
  до отчёта пользователю — не просто «когда удобно».
- Формат: дата `## YYYY-MM-DD` → маркированный список действий.

## Чистота проекта

- Корень проекта остаётся простым и читаемым.
- Исходники держать в `source\`.
- Release-артефакты держать в `build\Release\`.
- Промежуточные файлы держать в `build\obj\`.
- Reference-материалы — только в `reference\`, не в корне.
