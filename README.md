# OrcOutFit

Нативный **ASI-плагин** для **GTA San Andreas 1.0 US** (x86), в том числе для **SA:MP**. Рисует оружие на теле педа, кастомные объекты и кастомные скины поверх модели, с настройкой через **ImGui**-оверлей и INI-файлы.

**Репозиторий:** [github.com/dmitriyewich/OrcOutFit](https://github.com/dmitriyewich/OrcOutFit)

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

### Все педы в радиусе (опционально)
- Режим **«оружие у всех педов»** с радиусом от локального игрока.
- Отдельный кэш инстансов на каждого педа; пресеты оружия подбираются по **`GetPedStdSkinDffName`** (тот же DFF-кеш).

### Кастомные объекты
- Сканирование **`*.dff`** в **`OrcOutFit\Objects`**.
- Для каждого объекта — **`<имя>.ini`**. Настройки по стандартным скинам ped — только в секциях **`[Skin.<dff_name>]`** (имя DFF из `LoadPedObject`, без отдельных папок «под скин»).
- Масштаб: `Scale` и `ScaleX/Y/Z`; условный рендер по списку оружия (`Weapons`, `WeaponsMode`, `HideWeapons`) внутри секции `[Skin.*]`.

### Кастомные скины (DFF поверх педа)
- Сканирование **`*.dff`** в **`OrcOutFit\Skins`**.
- Рендер выбранного **clump** поверх локального ped, привязка анимации через **RpSkin** / иерархию базового ped.
- **Скрытие базового ped** (`CVisibilityPlugins::SetClumpAlpha`) — опция.
- **SA:MP: привязка по нику** — в INI скина (`[NickBinding]`).

### Интерфейс
- Вкладки **Main** (плагин, `[Features]`, пути), **Weapons**, **Objects**, **Skins**.
- Список стандартных ped для редактирования/примерки: **сортировка по model id по возрастанию**, подписи **`Имя [ID]`**.
- **Wear this skin (local player)** — смена модели локального педа для превью: стриминг (`RequestModel` / `LoadAllRequestedModels`), затем `CPed::SetModelIndex` в начале следующего кадра (безопасно для clump).
- **Save to skin (`OrcOutFit\Weapons`)** недоступен только в **одиночной игре** для **дефолтного CJ** (`MODEL_PLAYER`); в SA:MP и после примерки другой модели сохранение доступно.
- В редакторе оружия: **Copy/Paste** у `Show on body`.

### SA:MP
- При **известной** сборке `samp.dll`: чат-команда на меню, ник по педу, курсор через **`SetCursorMode`**.
- Неизвестная сборка: поведение как в одиночной игре (клавиша из INI).

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
3. Рядом с ASI появится **`OrcOutFit.ini`**.
4. Каталоги (относительно ASI):
   - `OrcOutFit\Objects` — кастомные объекты + их INI
   - `OrcOutFit\Weapons` — опциональные пресеты оружия `<dff>.ini`
   - `OrcOutFit\Skins` — кастомные скины DFF/TXD

---

## Сборка

- Visual Studio + **MSBuild**, платформа решения: **`x86`** (в `.vcxproj` — Win32).

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" OrcOutFit.sln /p:Configuration=Release /p:Platform=x86
```

Артефакт: **`build\Release\OrcOutFit.asi`** (каталог `build/` в `.gitignore`; после сборки появляется локально).

Зависимости: **plugin-sdk**, **imgui**, **MinHook** в `source\external\`.

### `Plugin.lib` (plugin-sdk)

- Ожидаемый путь: `source\external\plugin-sdk\output\lib\Plugin.lib`
- Если линковка ругается, сгенерируйте проекты premake и соберите `Plugin_SA` (Release|Win32). Подробности см. в истории проекта / `plugin-sdk` README.

---

## Меню и горячие клавиши

| Режим | Открытие |
|--------|----------|
| Одиночная игра | **`[Main] ActivationKey`** (по умолчанию **F7**) |
| SA:MP | **`[Main] Command`** (по умолчанию **`/orcoutfit`**). Опционально **`SampAllowActivationKey=1`** |

Удержание **ПКМ** отдаёт камере игры (меню может оставаться открытым).

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
| `RenderAllPedsRadius` | Радиус (метры) |
| `ConsiderWeaponSkills` | Dual wield по навыку |
| `CustomObjects` | Рендер объектов из `Objects` |
| `SkinMode` | Режим кастомных скинов из `Skins` |
| `SkinHideBasePed` | Скрыть базовый clump |
| `SkinNickMode` | Привязка скинов по нику SA:MP |
| `SkinLocalPreferSelected` | Локально предпочитать выбранный в UI скин (см. логику в коде) |

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

---

## Поддержка SA:MP

Детект по entry point `samp.dll`; для известных клиентов — хук команды, ники, `SetCursorMode`.

---

## Структура исходников

| Путь | Назначение |
|------|------------|
| `source/main.cpp` | Рендер, конфиг, хуки, streaming, скины |
| `source/orc_ui.cpp` | ImGui |
| `source/overlay.cpp` | D3D9 + ввод |
| `source/samp_bridge.cpp` | SA:MP |

---

## Ограничения

- Ориентация на **GTA SA 1.0 US** (адреса в коде).
- Не копируйте ASI в игру, пока процесс запущен и держит файл.

---

## Автор

**[@dmitriyewich](https://github.com/dmitriyewich)** — репозиторий [OrcOutFit](https://github.com/dmitriyewich/OrcOutFit). Лицензия: см. `LICENSE` в репозитории, если добавлен.
