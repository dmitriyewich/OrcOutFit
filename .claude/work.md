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
- Добавлена SA:MP-ориентированная активация UI для версий `R1`, `R2`, `R3`,
  `R3-1`, `R4`, `R4-2`, `R5-1`, `DL-R1`:
  - версия `samp.dll` определяется по `AddressOfEntryPoint`;
  - для каждой ревизии используется свой `sendCommand` offset;
  - установлен inline detour `sendCommand`, перехватывающий команду открытия меню.
- Новая логика активации:
  - вне SA:MP — только клавиша (по умолчанию `F7`);
  - в SA:MP — команда (по умолчанию `/orcoutfit`);
  - опция `Main.SampAllowActivationKey=1` дополнительно включает горячую клавишу в SA:MP.
- Расширен `OrcOutFit.ini`:
  - `Main.Command=/orcoutfit`
  - `Main.ActivationKey=F7` (поддержаны `F1..F12`, `A..Z`, `0..9`, либо числовой VK-код)
  - `Main.SampAllowActivationKey=0`
- `overlay` переведён с жёсткого `F7` на настраиваемый VK и флаг `hotkey enabled`,
  маршрутизация обновляется после загрузки INI и после детекта SA:MP.
- Проведён рефакторинг SA:MP-слоя в отдельный модуль:
  - добавлены `source/samp_bridge.h` и `source/samp_bridge.cpp`;
  - в модуль вынесены: детект `samp.dll`, определение ревизии по `EntryPoint`,
    таблица версий `R1/R2/R3/R3-1/R4/R4-2/R5-1/DL-R1`, inline detour `sendCommand`,
    и обработка команды активации;
  - `main.cpp` оставляет только интеграцию (`samp_bridge::Poll(...)`) и маршрутизацию
    горячей клавиши через `RefreshActivationRouting`.
- `OrcOutFit.vcxproj` обновлён: в сборку добавлен `source\samp_bridge.cpp`.
- Исправлен краш при смене скина SA:MP во время активного `Skin mode`:
  - причина: между `pedRenderEvent.before` и `pedRenderEvent.after` SA:MP мог
    заменить `m_pRwClump`, а код пытался восстановить callback/цвета по уже
    невалидным указателям атомиков/материалов;
  - добавлен guard snapshot-состояния (`ped + clump`), восстановление теперь
    выполняется только если clump не сменился;
  - при смене clump восстановление пропускается безопасно (очистка snapshot без
    dereference старых указателей) с диагностическим логом;
  - восстановление в `after` выполняется даже если toggles уже выключены, чтобы
    не оставлять незавершённый snapshot;
  - добавлены `__try/__except` вокруг hide/restore и полный сброс snapshot на
    `shutdownRwEvent`.
- После повторного отчёта о краше (`Illegal instruction`) путь скрытия базового
  ped дополнительно упрощён до безопасного режима:
  - убрана подмена/восстановление `RpAtomic::renderCallBack` при `Hide base ped`;
  - скрытие выполняется только через временный `material alpha=0` с последующим
    восстановлением цвета;
  - сохранены guards по `ped+clump` и безопасный skip restore при смене clump.
  Это исключает риск исполнения битого callback-указателя при асинхронной смене
  clump со стороны SA:MP.
- После ещё одного краша на смене скина SA:MP (`Access violation writing 0x0`)
  скрытие базового скина переведено на `CVisibilityPlugins::SetClumpAlpha`:
  - полностью удалены операции обхода/модификации материалов в `pedRenderEvent`;
  - `before`: `SetClumpAlpha(oldClump, 0)`;
  - `after`: восстановление `SetClumpAlpha(..., 255)` для старого clump и
    принудительное восстановление текущего `ped->m_pRwClump`, если SA:MP успел
    подменить clump между `before/after`;
  - сохранён snapshot guard (`ped/clump`) и `__try/__except`.
- В `RenderSelectedSkin` добавлен дополнительный guard на стабильность
  `player->m_pRwClump` в пределах кадра (ранний выход при подмене clump в ходе
  обработки), чтобы не продолжать bind/copy при гонке со сменой скина SA:MP.
- Найден и устранён ещё один вероятный источник краша при смене скина/командах:
  SA:MP command-hook был на самодельном inline detour (ручной trampoline с
  копированием первых байт функции), что небезопасно для сложных прологов/rel32.
  Путь заменён на MinHook.
  - `source/samp_bridge.cpp`: удалён `InstallSimpleDetour`, подключён
    `MH_Initialize/MH_CreateHook/MH_EnableHook` для `sendCommand`.
  - `OrcOutFit.vcxproj`: в сборку добавлены исходники MinHook
    (`buffer.c`, `hook.c`, `trampoline.c`, `hde32.c`).
  Это исключает исполнение битого trampoline-адреса при вызовах `sendCommand`.
- Реализована привязка кастомных skin из `OrcOutFit\SKINS` к никам SA:MP:
  - для каждого skin читается `<skin>.ini` с секцией `[NickBinding]`:
    - `Enabled=0/1`
    - `Nicks=name1,name2,...`
  - добавлен резолв ника по `CPed*` через SA:MP (`IdFind + GetNameById`) для
    версий `R1`, `R2`, `R3`, `R3-1`, `R4`, `R4-2`, `R5-1`, `DL-R1`;
  - скины с активным `NickBinding` применяются к удалённым ped по совпадению ника.
- Локальный игрок:
  - добавлена настройка `SkinMode.LocalPreferSelected`:
    - `0` — если есть skin по твоему нику, используется он;
    - `1` — приоритет у выбранного в UI skin (можно использовать любой кастомный,
      даже если есть привязка по твоему нику).
  - добавлена настройка `SkinMode.NickMode` для включения/выключения ник-режима.
- ImGui (`skins mode // OrcOutFit`) расширен:
  - `Nick binding mode (SA:MP)`;
  - `For my nick use selected skin`;
  - per-skin `Bind this skin to nick(s)` + поле `Nick list (comma-separated)` +
    `SAVE SKIN INI`.
- Исправлен визуальный баг с черным кастомным skin: в `RenderSelectedSkin`
  добавлен явный расчёт освещения через
  `CPointLights::GenerateLightsAffectingObject` +
  `SetLightColoursForPedsCarsAndObjects`.
  Теперь skin не зависит от того, рендерились ли перед ним оружие/объекты.
- Текущая стабильная линия коммитов:
  - `3bcb1b0` — stable checkpoint до нового направления;
  - `4da738f` — all-peds radius + `weaponsetting` overrides.
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

## 2026-04-16

- Реализован non-uniform scale для кастомных объектов:
  - добавлены поля `ScaleX/ScaleY/ScaleZ` в `CustomObjectCfg` (при `Scale` как базовом).
  - итоговый рендер-масштаб: `sx = Scale*ScaleX`, `sy = Scale*ScaleY`, `sz = Scale*ScaleZ`.
  - сохранена обратная совместимость старых INI: при отсутствии `ScaleX/Y/Z` используются `1.0`.
- `main.cpp`:
  - `CreateDefaultObjectIniIfMissing`: теперь пишет `ScaleX=1.000`, `ScaleY=1.000`, `ScaleZ=1.000`.
  - `LoadObjectCfgFromIni`: чтение `ScaleX/Y/Z`.
  - `SaveCustomObjectIni`: запись `ScaleX/Y/Z`.
  - `RenderCustomObject`: заменён uniform scale на поосевой (`RwMatrixScale` с `sx/sy/sz`).
- `orc_ui.cpp`:
  - Objects/Local и Objects/Other: добавлены контролы `Scale X/Y/Z` (`DragFloat3`) рядом с `Scale`.
- Сборка: `MSBuild OrcOutFit.sln /p:Configuration=Release /p:Platform=x86` → `build\Release\OrcOutFit.asi`.

- Полностью убрана внутренняя система debug-логов из `main.cpp`:
  - удалены `g_logPath`, `Log(...)` и все вызовы `Log(...)` в weapon/ped/skin/object-путях и инициализации.
  - `LogInit` теперь только рассчитывает `g_iniPath` и пути `OrcOutFit\\object/other/SKINS`, без создания `.log`.
  - сохранены только критичные guards/SEH без записи в файл.
  - проект успешно пересобран: `build\Release\OrcOutFit.asi`.

- `main.cpp`: исправлен хук `CFileLoader::LoadPedObject` (`0x5B7420`) под реальный формат строки
  `modelId dffName txdName ...` (пример: `235 SWMORI SWMORI ...`), а не прежний парсинг первого токена как имени модели.
  Детур теперь парсит `id/dff/txd`, использует `modelId` из возврата/строки и заполняет кеши `modelId -> dff/txd`.
- Хук `LoadPedObject` ставится в `DllMain` вместе с `LoadWeaponObject`, чтобы не пропускать однократную загрузку `ped.dat` до первого кадра.
- Починен `Reload INI` для оружия: убрана привязка загрузки секции к условию `Bone != 0`;
  добавлен `HasWeaponSection(...)`, поэтому секции читаются даже при `Bone=0` (например, если правятся только offsets/rot/scale).
- `Save weapon`/`Save all weapons`: сохранение теперь дублируется в числовые секции
  `[WeaponNN]` / `[WeaponNN_2]` помимо именованных (`[Name]` / `[Name2]`) для стабильного последующего `Reload INI`.
  Также расширен буфер имени секции и добавлены проверки границ `wt` в `SaveWeaponSection*`.
- Сборка: `MSBuild OrcOutFit.sln /p:Configuration=Release /p:Platform=x86` → `build\Release\OrcOutFit.asi`.

- `main.cpp`: исправлен хук `CFileLoader::LoadWeaponObject` (`0x5B3FB0`): строка, которую получает игра, имеет вид
  `modelId dffName txdName ...` (например `346 colt45 colt45 colt45 1 30 0`), а не «имя типа» в первом токене.
  Детур вызывает оригинал первым, берёт `modelId` из возврата/строки, `wt` через `FindWeaponType(dff)` (с попыткой upper-case),
  при неудаче — обратный поиск по `GetWeaponInfo` / `aWeaponInfo` по `m_nModelId` / `m_nModelId2`.
  Кеш `g_weaponDatIdeName[wt]` хранит basename из второго токена для подписи в UI.
- Хук `LoadWeaponObject` ставится уже в `DllMain` после `LogInit`, чтобы не пропустить однократную загрузку `weapon.dat` до первого `drawingEvent`.
- Скан типов: подписи из IDE-имени при наличии; для `wt > MAX_WEAPON_INFOS-1` догрузка model/model2 через `GetWeaponInfo` в SEH;
  fallback при пустом списке расширен с `68` до `min(255, MAX_WEAPON_INFOS-1)`.
- Сборка: `MSBuild OrcOutFit.sln /p:Configuration=Release /p:Platform=x86` → `build\Release\OrcOutFit.asi`.

- `context.md`: MSBuild — в `OrcOutFit.sln` платформа **`x86`** (не `Win32`); пример команды в контексте.
- `overlay.cpp`: ImGui/input курсора только при открытом меню; `ImGuiConfigFlags_NoMouseCursorChange`.
- `main.cpp`: один оверлей **OrcOutFit** с вкладками **Weapons / Objects / Skins**; полноширинные контролы, скролл окна, `TextWrapped` для путей; ники — `InputTextMultiline`.
- Сборка: `MSBuild` из пути в `context.md`, `/p:Platform=x86` → `build\Release\OrcOutFit.asi`.
- Вынесен ImGui: `orc_types.h` (WeaponCfg / custom cfg / кости / `D2R`), `orc_app.h` (extern состояние и API), `orc_ui.cpp` + `orc_ui.h` (`OrcUiDraw`); `main.cpp` подключает колбэк `OrcUiDraw`.
- Убран периодический лог `skin hide base ped` из `pedRenderEvent.before`.
- `orc_ui.cpp`: поля combo/drag/slider — фикс. ширина `kFieldW` (подписи не обрезаются при растягивании окна).
- `ParseNickCsv`: ники по **запятой и/или новой строке**; подпись multiline и комментарии ini обновлены.
- `orc_ui`: без `AlwaysVerticalScrollbar`, высота окна по умолчанию 560.
- `overlay`: при открытом меню `PatchCursor` и в SA:MP (без центрирования мыши); `ShowCursor` D3D только вне SA:MP; `WM_SETCURSOR` в SA:MP не трогаем.

- Кастомные объекты: добавлен `OrcOutFit\\object\\other\\<modelName>\\` для скино-зависимых пакетов (по стандартной ped model).
  - `<modelName>` мапится на модель локального игрока через `CPed->m_nModelIndex` / `CModelInfo->m_nKey`.
  - Внутри `<modelName>`: `*.dff` рендерятся как кастомные объекты для локального ped (и не зависят от `CustomSkinCfg.name`).
  - Там же поддержан `weapons.ini` — оверрайды позиций оружия для локального игрока под эту стандартную модель (замена прежней папки `weaponsetting`).
  - `Reload INI`/`Rescan objects` в UI теперь пересканируют и `object\\other`.

- `orc_ui.cpp` (`Objects` вкладка): добавлены подвкладки `Local` и `Other`.
  - `Local`: редактирование кастомных объектов из `OrcOutFit\\object\\`.
  - `Other`: список папок `object\\other` и редактирование кастомных объектов выбранного скина; добавлен `Rescan this skin objects`.

- `orc_ui.cpp` (`Weapons` вкладка): добавлен блок per-skin weapon offsets (`object\\other\\<skin>\\weapons.ini`) с подвкладками `Local skin` и `Other skin`.

- `Weapons`: учёт навыков оружия (dual wield).
  - `Main.ConsiderWeaponSkills=1` включает второй ствол для `bTwinPistol` при `WEAPSKILL_PRO`.
  - Добавлены отдельные конфиги второго ствола: секции `WeaponNN_2` / `[Pistol2]`, и per-skin `weaponCfg2` в `object\\other\\<skin>\\weapons.ini`.

- `Objects`: условный рендер кастомных объектов по наличию оружия.
  - Для каждого кастомного объекта (`<obj>.ini`) добавлены ключи:
    - `Weapons=` (csv weapon ids), `WeaponsMode=any|all`.
    - `HideWeapons=1` — скрывать выбранное оружие на теле, когда объект рендерится.
  - В UI добавлен multi-select список оружия и переключатель any/all + hide.

- По запросу пользователя зафиксированы постоянные правила работы:
  - в начало `context.md` добавлен раздел `Правила работы (обязательно)`;
  - добавлено правило Cursor `.cursor/rules/orcoutfit-workflow.mdc` (always apply):
    - обязательно обновлять `.claude/work.md` после заметных изменений;
    - не делать `git push` без явной команды пользователя;
    - синхронизировать документацию с кодом по запросу;
    - после существенных правок делать проверочную `Release|Win32` сборку.

