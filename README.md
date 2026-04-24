# OrcOutFit

Нативный **ASI-плагин** для **GTA San Andreas 1.0 US** (x86), в том числе для **SA:MP**. Рисует оружие на теле педа, кастомные объекты и кастомные скины поверх модели, с настройкой через **ImGui**-оверлей и INI-файлы.

**Репозиторий:** [github.com/dmitriyewich/OrcOutFit](https://github.com/dmitriyewich/OrcOutFit) — исходники, проект Visual Studio и файлы **`README.md`**, **`README.txt`**, **`context.md`**. Журнал агента (`.claude/work.md`) и правила Cursor остаются локальными.

---

## Возможности

### Оружие на теле
- Отображение моделей оружия на выбранной **кости** (RpHAnim node ID), со смещением, поворотом и масштабом.
- Оружие в **активном слоте** (в руках) на теле **не дублируется**.
- Секции по имени оружия из игры и резервные секции `[WeaponNN]` для кастомных типов.
- **Dual wield (skills)**: при прокачке навыка (PRO) и поддержке типа оружия игра может давать второй ствол; плагин рендерит и настраивает **вторую копию** (секции `WeaponNN_2` / `[Name2]`).
- **Авто-скан оружия**: хук `CFileLoader::LoadWeaponObject (0x5B3FB0)` (MinHook) по `weapon.dat` → `wt/modelId`, с fallback на `CWeaponInfo`; в UI: `Name [weaponTypeId][modelId]`.
- **Авто-стриминг** модели оружия, если она ещё не загружена.

### Переопределение оружия по стандартному ped (имя DFF из ped.dat)
- Полный пресет как у `OrcOutFit.ini` в файле **`OrcOutFit\Weapons\<имя_dff>.ini`** (поиск файла без учёта регистра).
- Имя скина берётся из кеша хука **`CFileLoader::LoadPedObject (0x5B7420)`** (`modelId → dff`).
- Приоритет: пресет скина выше глобального `OrcOutFit.ini`.
- Резолв `Weapons\<skin>.ini` кеширует найденные пути и отсутствующие файлы, чтобы режим всех ped не сканировал папку каждый кадр.

### Все педы в радиусе (опционально)
- Режимы **«оружие у всех ped»** и **«объекты у всех ped»** с радиусом от локального игрока.
- Для оружия используется отдельный per-ped cache инстансов; пресеты оружия подбираются по **`GetPedStdSkinDffName`**.
- Для объектов на каждом ped применяются секции `[Skin.<dff>]` из object INI; найденные секции и отсутствующие пары object/skin кешируются, чтобы режим не перечитывал INI каждый кадр.

### Кастомные объекты
- Сканирование **`*.dff`** в **`OrcOutFit\Objects`**.
- Для каждого объекта — **`<имя>.ini`**. Настройки по стандартным скинам ped — только в секциях **`[Skin.<dff_name>]`** (имя DFF из `LoadPedObject`, без отдельных папок «под скин»).
- Масштаб: `Scale` и `ScaleX/Y/Z`; условный рендер по списку оружия (`Weapons`, `WeaponsMode`, `HideWeapons`) внутри секции `[Skin.*]`.
- Подготовка материалов объектов выполняется один раз при загрузке инстанса; per-frame рендер больше не обходит все материалы объекта.

### Кастомные скины (DFF поверх педа)
- Сканирование **`*.dff`** в **`OrcOutFit\Skins`**.
- Рендер выбранного **clump** поверх ped, привязка анимации через **RpSkin** / иерархию базового ped.
- Цепочка освещения скина: **`CPed::SetupLighting`** (обёртка с SEH) → **`ApplyAttachmentLightingForPed`** с **`colourScale=1.0`** → **`RpClumpRender`** → при успешном **`SetupLighting`** — **`RemoveLighting`**. У оружия и кастом-объектов в той же функции освещения используется **`colourScale=0.5`**, чтобы уменьшить пересвет. Без отдельного **`PrepAtomicCB`** для скина.
- **Скрытие базового ped** (`CVisibilityPlugins::SetClumpAlpha`) — опция.
- **SA:MP** при включённом **`SkinNickMode`**: скин в основном **по нику** (`[NickBinding]` в INI скина); совпадение ника важнее выбранного в списке скина.
- Nick binding использует кеш `nick → skin`, а выбранный UI-скин кешируется по имени; кеш сбрасывается при рескане/сохранении и live-правках nick binding.
- **Локальный игрок:** **`SkinLocalPreferSelected`** (*Skin: always use selected skin for me*) включает выбранный в UI скин **в том числе при выключенном nick binding**; при включённом nick binding скин по совпадению ника по-прежнему **важнее**. Без этой опции и без ник-привязки оверлей на себя не ставится.

### Texture remap стандартных ped
- Подвкладка **Skins → Texture** включает PedFuncs-style замену текстур стандартных ped-моделей.
- Если в TXD модели есть текстуры вида **`*_remap`**, плагин ищет базовую текстуру с именем до `_remap` и временно подменяет её в материалах ped перед рендером.
- Для локального ped в UI доступны только реально найденные уникальные варианты, **Randomize local** и возврат к **Original textures**; PedFuncs-циклы по несуществующим номерам не используются.
- **Random mode**: `Linked variant` (по умолчанию) выбирает общий remap-index для всего ped, а если у отдельного слота такого индекса нет — берёт случайный реальный вариант этого слота. Повтор того же варианта подряд не запрещается.
- **Nick binding** для texture remap: текущий набор выбранных текстур можно сохранить для ника SA:MP. Binding имеет приоритет; если для ника binding не найден, используется random.
- Binding-файлы: **`OrcOutFit\Skins\Textures\<dff>.ini`**. Внутри сохраняются реальные имена original/remap-текстур, а не порядковые номера.
- Дополнительные `peds1..peds4.txd` кешируются через hook `AssignRemapTxd`, а поиск текстур расширяется hook-ом `RwTexDictionaryFindNamedTexture`, как в логике PedFuncs.

### Отладочный лог
- Файл **`OrcOutFit.log`** создаётся **рядом с `OrcOutFit.ini`** (не обязательно рядом с ASI при modloader — путь считается от INI).
- Уровни в **`[Features] DebugLogLevel`**: **`0`** — выкл.; **`1`** — только ошибки (префикс **`[E]`** в логе); **`2`** — полный trace (**`[I]`** + ошибки).
- Устаревший ключ **`DebugLog=1`** по-прежнему включает уровень **`2`** (если **`DebugLogLevel`** не задан).
- Настройка в UI: **Main** → **Debug log** (выпадающий список). Реализация: **`source/orc_log.cpp`**, **`source/orc_log.h`**.

### Интерфейс
- Вкладки **Main** (плагин, `[Features]`, пути), **Weapons**, **Objects**, **Skins**.
- Внутри **Skins** две подвкладки: **Custom skins** (прежняя логика DFF поверх ped) и **Texture** (`*_remap` для стандартных ped TXD).
- Список стандартных ped для редактирования/примерки: **сортировка по model id по возрастанию**, подписи **`Имя [ID]`**.
- **Wear this skin (local player)** — смена модели локального педа для превью: стриминг (`RequestModel` / `LoadAllRequestedModels`), затем `CPed::SetModelIndex` в начале следующего кадра (безопасно для clump).
- Во вкладке **Weapons** оставлены только две кнопки сохранения:
  - **Save to Global (`OrcOutFit.ini`)** — сохраняет весь набор оружия (primary + secondary) в глобальный INI.
  - **Save to skin (`OrcOutFit\Weapons`)** — сохраняет весь набор оружия (primary + secondary) в `Weapons\<dff>.ini`.
- **Save to skin (`OrcOutFit\Weapons`)** недоступен только в **одиночной игре** для **дефолтного CJ** (`MODEL_PLAYER`); в SA:MP и после примерки другой модели сохранение доступно.
- В редакторе оружия: **Copy/Paste** у `Show on body`.
- В редакторах **Weapons** и **Objects** включён **live preview**: offset/rotation/scale применяются сразу в рендере без обязательного сохранения.

### SA:MP
- При **известной** сборке `samp.dll`: чат-команда на меню, ник по педу, курсор через **`SetCursorMode`**.
- Для **локального** педа на части клиентов `IdFind` не находит слот; тогда используется **id локального игрока** из пула (если известен оффсет в таблице версий). Ник из пула **обрезается** по пробелам по краям.
- Неизвестная сборка: поведение как в одиночной игре (клавиша из INI).
- Флаг **`SampAllowActivationKey`** и поле **`ActivationKey`** применяются сразу при изменении в UI (без обязательного `Save main / features`).

---

## Требования

| Параметр | Значение |
|----------|----------|
| Игра | GTA San Andreas **1.0 US** |
| Архитектура | **Win32 (x86)** |
| SA:MP | Опционально; часть функций только при **поддерживаемой** `samp.dll` |

---

## Установка

1. Соберите проект (см. [Сборка](#сборка)) или возьмите готовый **`OrcOutFit.asi`**.
2. Положите **`OrcOutFit.asi`** в каталог с игрой (или modloader — пути данных считаются **от каталога ASI**).
3. Рядом с ASI появится **`OrcOutFit.ini`** (и при включённом логировании — **`OrcOutFit.log`**).
4. Каталоги (относительно ASI):
   - `OrcOutFit\Objects` — кастомные объекты + их INI
   - `OrcOutFit\Weapons` — опциональные пресеты оружия `<dff>.ini`
   - `OrcOutFit\Skins` — кастомные скины DFF/TXD
   - `OrcOutFit\Skins\Textures` — nick bindings для texture remap `<dff>.ini`

---

## Сборка

- Visual Studio + **MSBuild**, платформа решения: **`x86`** (в `.vcxproj` — Win32).

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" OrcOutFit.sln /p:Configuration=Release /p:Platform=x86
```

Артефакт: **`build\Release\OrcOutFit.asi`** (каталог `build/` в `.gitignore`; после сборки появляется локально).

Зависимости: **plugin-sdk**, **imgui**, **MinHook** в `source\external\`.

### GitHub Actions (Release Win32)

- Workflow: **`.github/workflows/build-release-win32.yml`**.
- Триггеры: ручной запуск (`workflow_dispatch`) и публикация релиза (`release.published`).
- Сборка в CI: сначала `source/external/plugin-sdk/plugin_sa/Plugin_SA.vcxproj` (`Release|Win32`, `PlatformToolset=v143`) для `Plugin.lib`, затем `OrcOutFit.sln` (`Release|x86`, `PlatformToolset=v143`).
- Публикация:
  - workflow artifact: `OrcOutFit-Release-Win32`;
  - release asset: `build/Release/OrcOutFit.asi`.

### `Plugin.lib` (plugin-sdk)

- Ожидаемый путь: `source\external\plugin-sdk\output\lib\Plugin.lib`
- В CI `Plugin.lib` собирается автоматически из vendored `plugin-sdk`; локально при ошибке линковки соберите `Plugin_SA` (`Release|Win32`) или используйте основной `.sln`-сценарий сборки после подготовки plugin-sdk.

---

## Меню и горячие клавиши

| Режим | Открытие |
|--------|----------|
| Одиночная игра | **`[Main] ActivationKey`** (по умолчанию **F7**) |
| SA:MP | **`[Main] Command`** (по умолчанию **`/orcoutfit`**). Опционально **`SampAllowActivationKey=1`** |

Удержание **ПКМ** отдаёт камере игры (меню может оставаться открытым).
Для стабильности курсора/drag в SA:MP используется sticky-capture + периодический reassert `SetCursorMode`.

---

## `OrcOutFit.ini`

### `[Main]`
| Ключ | Описание |
|------|----------|
| `Enabled` | Плагин вкл/выкл |
| `ActivationKey` | Клавиша (SP и опционально SA:MP) |
| `SampAllowActivationKey` | В SA:MP также разрешить клавишу (`0`/`1`) |
| `Command` | Чат-команда (с `/`) |

### `[Features]`
| Ключ | Описание |
|------|----------|
| `RenderAllPedsWeapons` | Оружие у всех ped в радиусе |
| `RenderAllPedsObjects` | Кастомные объекты у всех ped в радиусе |
| `RenderAllPedsRadius` | Радиус (метры) |
| `ConsiderWeaponSkills` | Dual wield по навыку |
| `CustomObjects` | Рендер объектов из `Objects` |
| `SkinMode` | Режим кастомных скинов из `Skins` |
| `SkinHideBasePed` | Скрыть базовый clump |
| `SkinNickMode` | Привязка скинов по нику SA:MP |
| `SkinLocalPreferSelected` | `1` — всегда использовать **выбранный в UI** скин на локальном игроке (если нет скина по нику); `0` — только ник |
| `SkinTextureRemap` | Включить PedFuncs-style texture remap для стандартных ped TXD (`*_remap`) |
| `SkinTextureRemapNickMode` | Включить приоритетные texture-remap привязки по нику SA:MP |
| `SkinTextureRemapRandomMode` | `0` = `Per texture`, `1` = `Linked variant` (дефолт) |
| `DebugLogLevel` | `0` / `1` (ошибки) / `2` (info+ошибки); см. раздел «Отладочный лог» |
| `DebugLog` | Legacy: `1` = то же, что **`DebugLogLevel=2`**, если уровень не задан отдельно |

### Секции оружия (корень INI)
Как раньше: именованные (`[M4]`) и `[WeaponNN]`, для второго ствола — `[WeaponNN_2]` / `[Name2]`.

### `[SkinMode]`
| Ключ | Описание |
|------|----------|
| `Selected` | Имя выбранного кастомного скина (basename `.dff`) |
| `RandomFromPools` | Ключ совместимости; скан random-пулов в текущей версии отключён |

---

## INI объекта (`OrcOutFit\Objects\<имя>.ini`)

Секции **`[Skin.<dff_name>]`** (имя как в ped.dat / кеше `LoadPedObject`): `Enabled`, кость, offset, rotation, scale, `ScaleX/Y/Z`, опционально `Weapons` / `WeaponsMode` / `HideWeapons`.

---

## INI кастомного скина (`OrcOutFit\Skins\<имя>.ini`)

**`[NickBinding]`**: `Enabled`, `Nicks` (через запятую и/или новую строку, без учёта регистра).

## INI texture binding (`OrcOutFit\Skins\Textures\<dff>.ini`)

Секции **`[Binding.N]`**: `Enabled`, `Nicks`, `SlotCount`, пары `SlotNOriginal` / `SlotNRemap`. Новые binding-и имеют больший `N` и при совпадении ника перекрывают старые.

---

## Поддержка SA:MP

Детект по entry point `samp.dll`; для известных клиентов — хук команды, ники, `SetCursorMode`.

---

## Структура исходников

| Путь | Назначение |
|------|------------|
| `source/main.cpp` | Рендер, конфиг, основные хуки, streaming, скины |
| `source/orc_texture_remap.cpp`, `source/orc_texture_remap.h` | Texture remap стандартных ped TXD (`*_remap`) |
| `source/orc_log.cpp`, `source/orc_log.h` | Лог в файл, уровни Info/Error |
| `source/orc_app.h`, `source/orc_types.h` | Состояние плагина, типы |
| `source/orc_ui.cpp` | ImGui |
| `source/overlay.cpp` | D3D9 + ввод |
| `source/samp_bridge.cpp` | SA:MP |

---

## Ограничения

- Ориентация на **GTA SA 1.0 US** (адреса в коде).
- Не копируйте ASI в игру, пока процесс запущен и держит файл.

---

## Автор

**[@dmitriyewich](https://github.com/dmitriyewich)** — исходники и проект VS: [OrcOutFit](https://github.com/dmitriyewich/OrcOutFit).
