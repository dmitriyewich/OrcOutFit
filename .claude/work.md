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
