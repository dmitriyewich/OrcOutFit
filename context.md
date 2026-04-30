# Context

## Правила работы (обязательно)

- После каждого заметного изменения кода/логики/сборки обязательно обновлять `.claude/work.md` до ответа пользователю.
- Без явной команды пользователя не делать `git push` (исключение: пользователь прямо просит запушить).
- Идентификация стандартного ped для INI/пресетов: **имя DFF из хука `LoadPedObject`** (`modelId → имя` из `ped.dat`). Отдельные пользовательские пути вида `id###` для папок пресетов **не используются** (см. `OrcOutFit\Weapons\<dff>.ini` и `[Skin.<dff>]`).
- Для оружия использовать `weapon.dat`-first подход (хук `LoadWeaponObject`) с безопасным fallback.
- После существенных правок обязательно делать проверочную сборку `Release|Win32`.
- Локальная сборка использует `PlatformToolset=v145` из проекта Visual Studio 18; GitHub Actions намеренно переопределяет сборку на `PlatformToolset=v143`, потому что на `windows-latest` доступны v143 build tools. Не унифицировать эти toolset-ы без явной проверки CI.
- Публичную документацию (`README.md`) синхронизировать с текущей реализацией при изменении поведения, установки, сборки или пользовательских сценариев. Локальные заметки (`context.md`, `README.txt`, `AGENTS.md`) обновлять по необходимости, но не публиковать без явного запроса пользователя или строгой необходимости для публичной сборки.

## Публикация в Git

- Удалённый репозиторий `https://github.com/dmitriyewich/OrcOutFit` должен содержать только публично необходимые файлы: **исходный код** (`source/`, в т.ч. `source/external/`), **GitHub workflow** (`.github/workflows/`), **профиль/файлы сборки VS** (`OrcOutFit.sln`, `OrcOutFit.vcxproj`) и **`README.md`**.
- Всё остальное остаётся локальным по умолчанию. В коммиты **не включаются**: `AGENTS.md`, `.claude/`, `.cursor/rules/`, `context.md`, `README.txt` — кроме случая, когда пользователь явно попросил опубликовать файл или файл строго необходим для публичной сборки.
- Техническое исключение: `.gitignore` можно менять, когда это нужно, чтобы локальные файлы не попадали в Git.

## Назначение проекта

- `OrcOutFit` это нативный `ASI`-плагин для `GTA San Andreas / SA:MP`.
- Публичный репозиторий (минимальное дерево): `https://github.com/dmitriyewich/OrcOutFit`
- Плагин рендерит:
  - оружие на теле ped (локальный игрок + опционально все ped),
  - кастомные объекты из папки `Objects`,
  - кастомный skin-режим для локального игрока из папки `Skins`,
  - PedFuncs-style texture remap для стандартных ped TXD по текстурам `*_remap`.
- Оружие, которое сейчас в руках у ped, на теле не рисуется.
- Выходной артефакт проекта:
  - `OrcOutFit.asi`

## Целевая платформа

- GTA San Andreas `1.0 US`.
- Только `Win32` (x86).

## Текущая рабочая логика

- Рендер игровых attachment-ов (оружие, кастомные объекты, кастомные скины) остаётся в `Events::drawingEvent`.
- ImGui-оверлей рендерится отдельно через D3D9 hooks:
  - установка hook-ов запускается из первого `drawingEvent`, когда игра уже дошла до drawable-кадра;
  - основной путь — `Present`, fallback — `EndScene`, reset device обслуживается через `Reset`;
  - одиночная игра не ждёт полной инициализации SA:MP; SA:MP влияет только на дополнительные функции клиента и cursor API.
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
  - опция `[Features] ConsiderWeaponSkills`,
  - если у оружия `CWeaponInfo::m_nFlags.bTwinPistol` (проверка skill-info: `GetWeaponInfo(wt,2)` с fallback на `1`)
    и `ped->GetWeaponSkill(wt) == WEAPSKILL_PRO` —
    рендерится второй инстанс оружия,
  - проверка навыка делается для конкретного ped (локальный и все ped в режиме `RenderAllPedsWeapons`),
  - настройки второго ствола хранятся отдельно (`WeaponNN_2` / `[Pistol2]`, etc.).
- Оружие всех ped (опция):
  - включается флагом `RenderAllPedsWeapons`,
  - фильтруется по радиусу `RenderAllPedsRadius`,
  - используется per-ped cache инстансов по `CPools::GetPedRef`,
  - резолв `OrcOutFit\Weapons\<dff>.ini` кеширует найденный путь и отрицательные промахи, чтобы рендер всех ped не сканировал папку `Weapons` из per-frame пути.
- Кастомные объекты всех ped (опция):
  - включается отдельным флагом `RenderAllPedsObjects`,
  - использует тот же радиус `RenderAllPedsRadius`,
  - для каждого ped берётся его `dff` и секция `[Skin.<dff>]` из каждого object ini,
  - резолв object/skin параметров кеширует и найденные секции, и отрицательные промахи; кеш сбрасывается при `Reload INI`, сохранении object params, смене модели ped и `Rescan Objects`.
- UI:
  - вкладки **Main** (плагин, `[Features]`, пути), **Weapons**, **Objects**, **Skins**; внутри **Skins** есть подвкладки **Custom skins** и **Texture**;
  - в Main добавлены отдельные флаги **Render weapons for all peds** и **Render objects for all peds**;
  - слайдер `All peds radius` общий для обоих флагов;
  - список стандартных ped из кеша `LoadPedObject`: **сортировка по `model id` по возрастанию**, подписи **`Имя [ID]`** (Weapons и Objects);
  - **Wear this skin**: очередь смены модели локального педа — в начале `drawingEvent` выполняются `CStreaming::RequestModel` / `LoadAllRequestedModels`, проверка `MODEL_INFO_PED`, `ClearAll()` по прикреплённому оружию, затем `CPed::SetModelIndex` @ `0x5E4880`;
  - **Save to `OrcOutFit\Weapons`**: блокируется только в **single-player** для **дефолтного CJ** (`MODEL_PLAYER` + `PED_TYPE_PLAYER1` + нет `samp.dll`); иначе доступно (в т.ч. SA:MP и после примерки скина).
- UI оружия:
  - рядом с `Show on body` есть `Copy`/`Paste` для текущего `WeaponCfg`,
  - Paste валидирует буфер и может вставлять между primary/secondary (dual wield),
  - оставлены только две кнопки сохранения: `Save to Global (OrcOutFit.ini)` и `Save to skin (OrcOutFit\Weapons)`,
  - `Save to Global` сохраняет весь набор weapon cfg (primary+secondary) в глобальный INI; `Save to skin` — весь набор в `Weapons\<dff>.ini`.
  - сохранение настроек больше не использует серии `WritePrivateProfileStringA`: INI-изменения собираются в памяти и записываются одним проходом через temp-файл и rename; глобальное сохранение оружия одновременно пишет актуальные `[Main]`, `[Features]`, `[SkinMode]` и weapon sections.
- Кастомные объекты:
  - скан `*.dff` в `OrcOutFit\Objects`,
  - отдельный `<name>.ini` на каждый объект; для каждого стандартного скина ped — секция `[Skin.<dff_name>]` (имя из `LoadPedObject`, без `object\other`).
  - масштаб: общий `Scale` и дополнительные множители `ScaleX/ScaleY/ScaleZ` (по осям).
  - объект может быть условным по оружию (внутри секции `[Skin.*]`):
    - `Weapons=` (csv weapon ids),
    - `WeaponsMode=any|all`,
    - `HideWeapons=1` — при срабатывании условия скрывать выбранное оружие на теле.
  - live preview параметров из UI для выбранной пары `(object ini + skin dff)` применяется сразу в рендере до сохранения.
  - attachment-материалы объектов/оружия подготавливаются один раз при загрузке/создании инстанса (`InitAttachmentAtomicCB`); per-frame `PrepAtomicCB` больше не обходит все материалы.
- Разрешение имени модели ped:
  - хук `CFileLoader::LoadPedObject` (`0x5B7420`, MinHook) кеширует `modelId -> modelName` (dff из ped.dat);
  - для пользовательских путей/INI используется это имя; числовой fallback папок `id###` не используется.
- Skin mode (`[Features] SkinMode`):
  - скан `*.dff` в `OrcOutFit\Skins` (папка `Skins`, не `SKINS`),
  - при **SA:MP** и включённом **`[Features] SkinNickMode`**: кастомный clump выбирается **по нику** (`[NickBinding]` в INI скина); при совпадении ника он **важнее** выбранного в UI скина,
  - nick binding использует кеш `nick -> skin index`, а выбранный UI-скин кешируется по имени; кеш сбрасывается при рескане/сохранении skin INI и live-изменениях nick binding в UI,
  - **`[Features] SkinLocalPreferSelected`** (*always use selected*): на **локального игрока** вешается скин из вкладки Skins (`SkinMode`/`Selected`), если опция включена — **в том числе при выключенном `SkinNickMode`**; при включённом nick mode без совпадения ника поведение то же; при совпадении ника побеждает скин по `[NickBinding]`,
  - рендер выбранного clump поверх ped, скрытие базового ped (опция), bind через `RpSkinAtomicSetHAnimHierarchy`;
  - **кастомный скин:** загрузка как у прочих кастом-clump (`InitAtomicCB`); рендер: `CPed::SetupLighting` (SEH) → `ApplyAttachmentLightingForPed` с **colourScale=1.0** → `RpClumpRender` → при успешном `SetupLighting` — `RemoveLighting` (у оружия/объектов **colourScale=0.5**); без `PrepAtomicCB` для скина.
  - **Лог `OrcOutFit.log` (рядом с INI):** `[Features] DebugLogLevel` — `0` выкл., `1` только ошибки (`[E]`), `2` полный trace (`[I]` + ошибки). Ключ **`DebugLog=1`** (legacy) по-прежнему включает уровень 2. Реализация: `source/orc_log.cpp`; в UI: Main → **Debug log** (combo).
  - случайные пулы `Skins\random` **не сканируются**; `ResolveSkinForPed` не использует random-fallback,
  - ключ `RandomFromPools` в `[SkinMode]` в INI остаётся для совместимости.
- Skin texture remap (`[Features] SkinTextureRemap`):
  - подвкладка **Skins → Texture** включает PedFuncs-style замену материалов стандартного ped перед его рендером и восстановление сразу после `pedRenderEvent.after`;
  - для каждого ped сканируется TXD стандартной модели (`CBaseModelInfo::m_nTxdIndex`) и собираются пары `original` / `*_remap`; базовое имя берётся как часть до `_remap`;
  - список вариантов строится только из реально найденных уникальных texture names; PedFuncs-style циклическая индексация/повторы и несуществующие номера не используются;
  - поддерживается до 8 remap-слотов на модель, ручной выбор реального варианта, возврат к оригиналу и randomize;
  - `[Features] SkinTextureRemapRandomMode`: `0` = `Per texture`, `1` = `Linked variant` (дефолт); linked выбирает один общий remap-index для всех слотов, где он есть, а для слотов с меньшим числом вариантов берёт случайный реальный вариант;
  - random не блокирует повтор того же варианта подряд: повторы остаются естественной частью случайного выбора;
  - `[Features] SkinTextureRemapNickMode=1` включает привязки texture remap по нику SA:MP; binding всегда имеет приоритет, а если binding для ника не найден — остаётся random;
  - привязки хранятся в `OrcOutFit\Skins\Textures\<dff>.ini`, секции `[Binding.N]`: `Nicks`, `SlotCount`, `SlotNOriginal`, `SlotNRemap`; сохраняются реальные texture names, а не индексы;
  - хук `AssignRemapTxd` кеширует дополнительные `peds1..peds4.txd`, а хук `RwTexDictionaryFindNamedTexture` ищет недостающие текстуры в этих словарях;
  - состояние ped сбрасывается при `pedSetModelEvent`, выключении плагина/фичи и shutdown; во время cutscene-окна rescan откладывается;
  - реализация вынесена из `main.cpp` в отдельный модуль `source\orc_texture_remap.cpp/.h`; `main.cpp` только подключает hooks/events и конфиг.
- Per-skin weapon overrides (по имени DFF ped из ped.dat):
  - `OrcOutFit\Weapons\<skin>.ini` (регистр имени файла не важен),
  - полный набор секций оружия как в `OrcOutFit.ini`; приоритет выше глобального `OrcOutFit.ini`.
  - live preview в UI `Weapons` применяется сразу для выбранного скина (без сохранения), а после Save сбрасывается в постоянные данные.

- Курсор/ввод overlay:
  - стабильный UI-capture: sticky mouse capture между `WndProc` и `NewFrame`, принудительная синхронизация `MousePos` из OS при входе в overlay,
  - при активном UI курсоре используется `ImGui::SetNextFrameWantCaptureMouse(true)` для устранения «провалов» захвата,
  - удержание ПКМ временно отдаёт mouse messages и управление камере игры, даже если меню открыто,
  - в распознанном SA:MP `SetCursorMode` реутверждается каждые 200 ms, чтобы курсор не отваливался при внешних сбросах,
  - в одиночной игре и неизвестных сборках SA:MP используется single-player fallback: runtime patch cursor call-sites + `IDirect3DDevice9::ShowCursor`,
  - перед single-player patch сохраняются реальные байты из памяти и при выключении курсора восстанавливается именно этот snapshot; после записи патча вызывается `FlushInstructionCache`,
  - cursor/player-control state применяется после восстановления D3D state block, а render detour обёрнут SEH guard: при fault меню закрывается, курсор и управление возвращаются игре.

- Активация меню:
  - в SA:MP по команде `[Main] Command`, клавиша дополнительно через `[Main] SampAllowActivationKey=1`,
  - изменение `ActivationKey` и `SampAllowActivationKey` в UI применяется сразу (через `RefreshActivationRouting`) без обязательного `Save main / features`.

## Важные файлы

- Основной код:
  - `C:\Games\CODEX\WeaponsOutFit\source\main.cpp`
  - UI (ImGui): `source\orc_ui.cpp`, `source\orc_ui.h`; overlay render/input/cursor: `source\overlay.cpp`, `source\overlay.h`; SA:MP bridge: `source\samp_bridge.cpp`, `source\samp_bridge.h`; общие типы: `source\orc_types.h`; мост к состоянию: `source\orc_app.h`; лог: `source\orc_log.cpp`, `source\orc_log.h`; texture remap + nick bindings: `source\orc_texture_remap.cpp`, `source\orc_texture_remap.h`
- MinHook:
  - `C:\Games\CODEX\WeaponsOutFit\source\external\MinHook\`
- Проект Visual Studio:
  - `C:\Games\CODEX\WeaponsOutFit\OrcOutFit.vcxproj`
- Solution:
  - `C:\Games\CODEX\WeaponsOutFit\OrcOutFit.sln`
- Конфиг:
  - рядом с ASI: `OrcOutFit.ini`
- Публичная документация в репозитории: `README.md`. Локальные заметки/правила: `context.md`, `README.txt`, `AGENTS.md`, журнал `.claude\work.md` и правила `.cursor\rules\` — только локально; публиковать их можно только по явному запросу пользователя или при строгой необходимости для публичной сборки.

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

## SA:MP: ник для скина

- Резолв ника: `samp_bridge::GetPedNickname` → `IdFind` + `GetNameById`. На части клиентов `IdFind` для **локального** `CPed*` возвращает `0xFFFF`; тогда для `gtaPed == FindPlayerPed(0)` подставляется **id локального игрока** из `CNetPlayerPool` (`localPlayerIdOffset` в таблице версий). Имя из SA:MP обрезается по пробелам по краям (`TrimSampNameInPlace`). Для клиентов с `localPlayerIdOffset == 0` в таблице fallback невозможен — нужно расширять оффсеты.

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
  - локально используется `PlatformToolset=v145` из `OrcOutFit.vcxproj`;
  - CI не должен наследовать локальный `v145`: workflow явно передаёт `PlatformToolset=v143`.
- Ожидаемый выходной файл:
  - `C:\Games\CODEX\WeaponsOutFit\build\Release\OrcOutFit.asi`
- GitHub Actions workflow:
  - `.github/workflows/build-release-win32.yml`
  - триггеры: `workflow_dispatch` и `release.published`
  - в CI сначала собирается `source/external/plugin-sdk/plugin_sa/Plugin_SA.vcxproj` (`Release|Win32`, `PlatformToolset=v143`) для `Plugin.lib`,
  - затем собирается `OrcOutFit.sln` (`Release|x86`, `PlatformToolset=v143`),
  - артефакт CI и release-asset: `build/Release/OrcOutFit.asi`

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
- Конфиг: `OrcOutFit.ini` рядом с ASI (или в каталоге загрузки ASI); **лог** `OrcOutFit.log` — рядом с этим INI (имя: тот же базовый путь, расширение `.log`).
- Ожидаемые подпапки:
  - `OrcOutFit\Objects`
  - `OrcOutFit\Weapons`
  - `OrcOutFit\Skins`
  - `OrcOutFit\Skins\Textures`

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
