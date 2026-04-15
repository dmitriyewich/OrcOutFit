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
