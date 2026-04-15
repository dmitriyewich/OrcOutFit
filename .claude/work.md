# Worklog

## 2026-04-14

- Проведена серия итераций по отладке рендера оружия на теле игрока.
- Ключевые багфиксы по ходу работы:
  - `CBaseModelInfo::CreateInstance(RwMatrix*)` переведён с `vtable[9]` на `vtable[10]`
    (по `plugin-sdk\CBaseModelInfo.cpp`).
  - `RpAtomic::renderCallBack` скорректирован с `+0x48` на `+0x44`
    (ранее запись в `+0x48` портила `inClumpLink.next`).
  - Кости переведены с индексов `m_apBones` на `BONE_* NODE IDs`:
    `BACK=3`, `HIP_LEFT=41`, `HIP_RIGHT=51`.
  - `GetBoneMatrix` теперь использует `RpHAnimIDGetIndex` напрямую по `NODE ID`.
  - Добавлен `FixInstanceCallbacks` — перебивает `renderCallBack`
    у всех атомиков инстанса на `AtomicDefaultRenderCallBack` (`0x7491C0`),
    чтобы рендер шёл немедленно, минуя alpha-sort.
- Убран спам в лог:
  - снят периодический per-frame лог,
  - ключевые события логируются через `static bool loggedX[64]` (по 1 разу на `wt`).
- Подтверждено по логу: все 8 оружий создаются без исключений,
  `RpClumpRender` вызывается с валидной матрицей, но визуально
  оружие на персонаже не появлялось при хуке `CPed::Render` (`0x5E7680`).
- Принято решение перейти на подход `BaseModelRender`:
  рендерить из глобального `drawingEvent`, а не из `CPed::Render`.
- Переделан хук:
  - удалён хук `CPed::Render` (`0x5E7680`).
  - добавлен хук `drawingEvent` по call-site `0x53E293`:
    на старте читается `E8 rel32`, вычисляется абсолютный адрес callee,
    и `MinHook` ставится уже на целевую функцию.
  - после вызова оригинала вызывается `SyncAndRender(FindPlayerPed(0))`.
- Выполнена сборка `Release | Win32`:
  - `C:\Games\CODEX\WeaponsOutFit\build\Release\WeaponsOutFit.asi`
- `.asi` скопирован в папку игры:
  - `C:\Games\SAMP\GTA San Andreas\WeaponsOutFit.asi`
- Далее — проверка в игре, правка оффсетов/ротаций при необходимости.

## 2026-04-15

- Зафиксировано правило: `.asi` только в `build\Release\`, автокопирование
  запрещено (см. context.md). Поднят локальный git, `.gitignore` добавлен.
- Подключён imgui v1.92.7 (скопирован из MyAsiModReshenie/external),
  плоско, без submodule. plugin-sdk аналогично расплющен.
- Добавлено окно imgui для настройки крепления оружия (`overlay.cpp/h`):
  комбо-выбор weapon, кости (20 штук), offset XYZ, rot XYZ (deg), scale,
  SAVE / SAVE ALL, крестик закрытия, тоггл F7.
- Курсор: патч plugin-sdk рецептом — NOP на `0x541DF5` / `0x53F417`,
  `xor eax,eax/jz` на `0x53F41F`, RET на `0x6194A0`; `dev->ShowCursor()` +
  `io.MouseDrawCursor`. Работает и в SAMP, и в singleplayer.
  При зажатом RMB курсор скрывается (look-around), меню остаётся видимым.
- Rot переведён на прямую ZYX-Euler матрицу (применяется к базису кости) —
  прежний `RwMatrixRotate`+PRECONCAT не работал.
- Ammo==0 для slot 2–9 отфильтровывается (слоты 0/1/10+ не требуют патронов).
- Shutdown-краш в `CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB`
  @ `0x5D95B0` глушится патчем первого байта на `0xC3` (RET) в
  `shutdownRwEvent` — плагин-данные к этому моменту уже невалидны,
  процесс всё равно завершается.
- **Агрессивный Release-профиль** по образу OpenSaa: `/GS-`, `/Gw`,
  `BufferSecurityCheck=false`, `RuntimeTypeInfo=false`,
  `ExceptionHandling=false` (SEH `__try/__except` продолжает работать),
  `OmitFramePointers`, `InlineAnySuitable`, `SSE2`, `StringPooling`,
  `LinkTimeCodeGeneration`. CRT не отключал — нужен imgui/plugin-sdk.
- Консолидация кода: слияние `ResetAtomicCB`+`LeakAtomicCB` → `InitAtomicCB`,
  `GeomMatWhiteCB`+`PrepareAtomicCB` → `WhiteMatCB`+`PrepAtomicCB`,
  убрана мёртвая `tmp[64]`-копия в `SaveDefaultConfig` и неиспользуемый
  `g_uiDirty` / `g_prevMenuOpen` / `PatchUpdateMouse`.
- Коммиты: `b3a7558` initial, `fd142a7` flatten plugin-sdk,
  `952099b` imgui overlay+editor, `2419a23` flatten imgui,
  `540ee5c` aggressive profile + consolidation.
- **Размер `.asi` — 664 КБ**: это почти вся imgui (core + draw + tables +
  widgets + dx9 backend + win32 backend, ~400 КБ) + собственный код и
  плагин-sdk хуки. CRT статический (`MultiThreaded`). Отдельно imgui
  ужать не предлагаю — можно при желании отключить `imgui_tables.cpp` и
  `imgui_demo.cpp` (уже не включён), но таблицы используются в будущих
  настройках. При необходимости — `/O1` вместо `MaxSpeed` или динамический
  CRT (`MultiThreadedDLL`) уменьшит размер ценой зависимости от vcruntime.
- Добавлено автообнаружение кастомных объектов из папки игры
  `C:\Games\SAMP\GTA San Andreas\WeaponsOutFit`:
  сканируются все `*.dff`, для каждого объекта формируется запись в runtime-
  списке и автоматически создаётся отдельный INI рядом с моделью
  (`<name>.dff` -> `<name>.ini`), если INI отсутствует.
- Формат авто-INI: секция `[Main]` с `Enabled`, `Bone`, `Offset*`,
  `Rotation*`, `Scale` (дефолтная кость — `BONE_R_THIGH`).
- Инициализация скана подключена в первый `drawingEvent` после загрузки
  основного конфига плагина.
- Реализован реальный рендер кастомных объектов (как в `BaseModelRender`) без
  сущностей/пулов: для каждого найденного `*.dff` объект загружается через
  plugin-sdk (`CTxdStore` + `CFileLoader::SetRelatedModelInfoCB` в
  `CAtomicModelInfo`), затем создаётся `CreateInstance(RwMatrix*)` и рендерится
  из `drawingEvent` через `RpClumpRender`/atomic callback.
- Для каждого кастомного объекта читается собственный `<name>.ini`
  (секция `[Main]`: `Enabled`, `Bone`, `Offset*`, `Rotation*`, `Scale`), а
  кнопка `Reload INI` теперь перезагружает и основной конфиг, и конфиги
  кастомных объектов из папки игры.
- Добавлена синхронная очистка инстансов кастомных объектов при отключении
  плагина, потере игрока и в `shutdownRwEvent`.
- ImGui расширен отдельным окном `custom objects // WeaponsOutFit`:
  выбор обнаруженного объекта (`*.dff`), редактирование `Enabled/Bone`,
  `OffsetX/Y/Z`, `RotationX/Y/Z`, `Scale`, сохранение в его `<name>.ini`
  (`SAVE OBJECT` / `SAVE ALL OBJECTS`) и перескан папки (`RESCAN`).
- В основном окне добавлена кнопка `Rescan Objects`; `Reload INI` теперь также
  пересканирует и перечитывает конфиги кастомных объектов.
- Исправлен поиск `.txd` для кастомных `*.dff`: вместо жёсткого `<base>.txd`
  добавлен поиск по папке с case-insensitive совпадением basename.
  Fallback: если в папке ровно один `.txd`, используется он.
  При отсутствии подходящего `.txd` объект пропускается с единичной записью в лог.
- Устранён источник ошибки `...Holster.txd cannot be found`: загрузка TXD
  переведена на потоковый путь (через `RwStreamOpen` + `CTxdStore::LoadTxd(slot, stream)`),
  что обходит проблемный файловый резолв `LoadTxd(slot, filename)` с абсолютными путями.
- После краша в `CAutomobile::SetupSuspensionLines` убран рискованный путь
  `CAtomicModelInfo + CFileLoader::SetRelatedModelInfoCB` для кастом-объектов.
  Кастомный `dff` теперь грузится и хранится как `RpClump` напрямую, без
  регистрации model info в игровых структурах, и рендерится напрямую на кости.
- Изменён путь скана кастом-объектов на
  `C:\Games\SAMP\GTA San Andreas\WeaponsOutFit\object`.
- Добавлен прототип `SKINS`-режима для локального игрока:
  скан папки `C:\Games\SAMP\GTA San Andreas\WeaponsOutFit\SKINS`,
  загрузка `dff/txd` в RW, выбор активного скина, рендер выбранного clump в
  `drawingEvent`, копирование позы по `RpHAnimHierarchy` из локального ped в
  кастомный clump.
- Добавлено скрытие базового локального скина в `pedRenderEvent.before/after`
  (через временный alpha=0 материалов с восстановлением после рендера).
- Добавлен INI-блок `[SkinMode]` в `WeaponsOutFit.ini`:
  `Enabled`, `HideBasePed`, `Selected`, плюс UI-кнопка сохранения.
- Исправления `SKINS`-режима после первого прогона:
  - анимация теперь копируется не по индексу матриц, а по `nodeID` через
    `RpHAnimIDGetIndex` (source->destination), затем `RpHAnimHierarchyUpdateMatrices`;
  - скрытие базового локального ped усилено: перед его рендером сохраняются
    и временно подменяются atomic render callbacks (`NoRenderAtomicCB`), после
    рендера callbacks восстанавливаются; alpha-скрытие материалов оставлено.
  - добавлены диагностические записи в `WeaponsOutFit.log` по состоянию
    скрытия ped и загрузки/рендера выбранного skin clump.
- Дополнительный фикс анимации кастомного skin:
  после обновления `dst->pMatrixArray` добавлено зеркалирование позы прямо в
  `RwFrame` узлов по `nodeID` (`src pNodeInfo[i].pFrame` -> `dst pNodeInfo[di].pFrame`)
  с `RwMatrixUpdate`. Это покрывает clump-и, которые рендерятся по frame-матрицам
  и ранее оставались в T-pose.
- Лог расширен: добавлен отчёт о количестве найденных скинов и одноразовый лог
  по выбранному skin (`src/dst hierarchy pointer + numNodes`).
- По логу выявлено: `dstH == nullptr` у выбранного skin, т.е. в DFF отсутствует
  skin/hanim hierarchy для анимации. Добавлен guard `g_skinCanAnimate`:
  базовый ped скрывается только когда у кастомного skin есть валидный hierarchy.
  При `dstH == nullptr` пишется явный лог о невозможности анимации для этого DFF.
- Переработан источник анимации для кастомного skin:
  вместо требования `dstH` у clump внедрена привязка `srcH` локального ped к
  skinned atomic-ам кастомного clump через `RpSkinAtomicSetHAnimHierarchy` +
  `RpSkinAtomicSetType(rpSKINTYPEGENERIC)`.
  `g_skinCanAnimate` теперь определяется по `bindCount>0`.
- Лог дополнен `skinAtomics` и сообщением о невозможности bind (`srcH/bindCount`).
- Выполнен ребрендинг проекта на `OrcOutFit`:
  - target binary: `OrcOutFit.asi` (vcxproj `TargetName`);
  - runtime файлы автоматически: `OrcOutFit.log` / `OrcOutFit.ini`;
  - обновлены UI заголовки на `OrcOutFit`;
  - игровые папки изменены на
    `C:\Games\SAMP\GTA San Andreas\OrcOutFit\object` и
    `C:\Games\SAMP\GTA San Andreas\OrcOutFit\SKINS`;
  - имя проекта в solution отображается как `OrcOutFit`.
- Файлы проекта физически переименованы:
  - `WeaponsOutFit.sln` -> `OrcOutFit.sln`
  - `WeaponsOutFit.vcxproj` -> `OrcOutFit.vcxproj`
  и ссылка внутри solution обновлена на новый `.vcxproj`.
- Изолирован `IntDir` для нового имени проекта:
  `build\obj\$(Configuration)\OrcOutFit\` (убран warning о shared intermediate dir).
- Для сценария modloader пути к данным переведены на вычисление относительно
  расположения `OrcOutFit.asi`:
  - `<asi-dir>\OrcOutFit\object`
  - `<asi-dir>\OrcOutFit\SKINS`
  Это позволяет размещать плагин в `modloader\OrcOutFit\...` без хардкода пути
  к корню игры.
- Добавлен переключаемый режим рендера оружия для всех ped:
  - новый флаг `Main.RenderAllPedsWeapons` в `OrcOutFit.ini` (по умолчанию `0`);
  - checkbox в UI: `Render weapons for all peds`;
  - сохранение флага через `SAVE`/`SAVE ALL`.
- Реализация: введён per-ped кэш инстансов оружия (`unordered_map<PedHandle, array<RenderedWeapon,13>>`)
  с синхронизацией по инвентарю каждого ped из `CPools::ms_pPedPool`.
  При выключении режима кэш полностью очищается; при исчезновении ped записи удаляются.
- Добавлен настраиваемый радиус для all-peds режима:
  - INI: `Main.RenderAllPedsRadius` (метры);
  - UI: `All peds radius`;
  - рендер/синхронизация выполняются только для ped внутри радиуса от локального.
- Добавлены per-skin weapon presets из папки
  `<asi-dir>\OrcOutFit\weaponsetting\`:
  - каждый `*.ini` мапится на модель ped по basename через `CKeyGen::GetUppercaseKey`
    (пример: `lapd1.ini` -> модель `lapd1` / skin id 280);
  - формат секций идентичен основному `OrcOutFit.ini` (`[WeaponNN]` и именованные секции);
  - при наличии override у конкретной модели ped используется его `WeaponCfg`,
    иначе глобальный дефолт.
- Для сценария modloader пути к данным переведены на вычисление относительно
  расположения `OrcOutFit.asi`:
  - `<asi-dir>\OrcOutFit\object`
  - `<asi-dir>\OrcOutFit\SKINS`
  Это позволяет размещать плагин в `modloader\OrcOutFit\...` без хардкода пути
  к корню игры.
