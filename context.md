# Context

## Правила работы (обязательно)

- После каждого заметного изменения кода/логики/сборки обязательно обновлять `.claude/work.md` до ответа пользователю.
- Без явной команды пользователя не делать `git push` (исключение: пользователь прямо просит запушить).
- Приоритет идентификации моделей/скинов: имя модели -> серверный/хук-кеш имени -> `id###/###` fallback.
- Для оружия использовать `weapon.dat`-first подход (хук `LoadWeaponObject`) с безопасным fallback.
- После существенных правок обязательно делать проверочную сборку `Release|Win32`.
- Документацию (`context.md`, `README.md`, `README.txt`) синхронизировать с текущей реализацией перед финальным коммитом по запросу пользователя.

## Назначение проекта

- `OrcOutFit` это нативный `ASI`-плагин для `GTA San Andreas / SA:MP`.
- Плагин рендерит:
  - оружие на теле ped (локальный игрок + опционально все ped),
  - кастомные объекты из папки `object`,
  - кастомный skin-режим для локального игрока из папки `SKINS`.
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
  - если у оружия `CWeaponInfo::m_nFlags.bTwinPistol` и `ped->GetWeaponSkill(wt) == WEAPSKILL_PRO` —
    рендерится второй инстанс оружия,
  - настройки второго ствола хранятся отдельно (`WeaponNN_2` / `[Pistol2]`, etc.).
- Оружие всех ped (опция):
  - включается флагом `RenderAllPedsWeapons`,
  - фильтруется по радиусу `RenderAllPedsRadius`,
  - используется per-ped cache инстансов по `CPools::GetPedRef`.
- Кастомные объекты:
  - скан `*.dff` в `OrcOutFit\object`,
  - отдельный `<name>.ini` на каждый объект.
  - объект может быть условным по оружию:
    - `Weapons=` (csv weapon ids),
    - `WeaponsMode=any|all`,
    - `HideWeapons=1` — при срабатывании условия скрывать выбранное оружие на теле.
- Кастомные объекты по стандартной модели ped:
  - папка `OrcOutFit\object\other\<skin>\` (где `<skin>` — имя стандартной модели, например `wmyclot`,
    либо `id217` / `217` для проектов с добавленными ped-models по id),
  - скан `*.dff` в каждой подпапке, `<name>.ini` на каждый объект.
- Разрешение имени модели ped (для `object\other` и `SKINS\random`):
  - хук `CFileLoader::LoadPedObject` (`0x5B7420`, MinHook) кеширует `modelId -> modelName`;
  - приоритет резолва папок: имя модели (`CModelInfo`) -> имя из кеша `LoadPedObject` -> `id###/###` fallback.
- Skin mode:
  - скан `*.dff` в `OrcOutFit\SKINS`,
  - рендер выбранного clump поверх локального ped,
  - скрытие базового ped (опция),
  - bind анимации через `RpSkinAtomicSetHAnimHierarchy`.
- Per-skin weapon overrides (по стандартной модели ped):
  - `OrcOutFit\object\other\<skin>\weapons.ini`,
  - применяются к локальному игроку по `CModelInfo->m_nKey`,
  - если overrides отсутствуют — используются глобальные дефолты из `OrcOutFit.ini`.

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
  - `OrcOutFit\object`
  - `OrcOutFit\object\other`
  - `OrcOutFit\SKINS`
  - `OrcOutFit\SKINS\random`

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
