# OrcOutFit

<img width="238" height="408" alt="kHKmcxq" src="https://github.com/user-attachments/assets/44e51b41-9600-4cb8-ae4e-611afc44ed08" />

Нативный **ASI-плагин** для **GTA San Andreas 1.0 US** (x86), в том числе для **SA:MP**. Рисует оружие на теле педа, кастомные и стандартные игровые объекты, кастомные/стандартные/random-скины поверх модели, умеет менять DFF/TXD оружия и отдельные TXD-текстуры оружия, с настройкой через **ImGui**-оверлей и INI-файлы.

**Репозиторий:** [github.com/dmitriyewich/OrcOutFit](https://github.com/dmitriyewich/OrcOutFit) — исходники, `.github/workflows`, профиль сборки Visual Studio и **`README.md`**. `context.md` и `README.txt` считаются локальными заметками по умолчанию и публикуются только по явному запросу; `AGENTS.md`, журнал агента (`.claude/work.md`) и правила Cursor остаются локальными.

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

### Замена моделей оружия
- Подвкладка **Weapons → Weapon replacement** управляет заменой моделей оружия, которое OrcOutFit рисует на теле, и видимого оружия в руках.
- Приоритет замены: **`OrcOutFit\Weapons\GunsNick\<weapon>_<nick>.dff`** выше замены по скину; ник сравнивается без учёта регистра и без SA:MP color-кодов.
- Замена по скину хранится в **`OrcOutFit\Weapons\Guns\<weapon>\<dff>.dff`**; random-варианты — в **`OrcOutFit\Weapons\Guns\<weapon>\<dff>\*.dff`**.
- Имя `<weapon>` берётся из `weapon.dat` / `LoadWeaponObject` и сравнивается без учёта регистра (`m4`, `ak47`, `desert_eagle` и т.п.).
- Random-вариант закрепляется за ped/оружием/скином и выбирается через shuffle-bag, без файловых сканов в per-frame пути.
- Оружие в руках при **Weapon replacement**: подмена `m_pWeaponObject` на кадр рендера ped и хуки **`CPed::AddWeaponModel`** / **`CPed::RemoveWeaponModel`** (SA 1.0 US: `0x5E5ED0` / `0x5E3990`): сразу после того как движок подцепил штатную модель к ped, в слот подставляется клон замены; при снятии модели указатель возвращается движку, клон уничтожается. На каждом кадре матрица клона по-прежнему копируется со штатного меша (после прошлого кадра в слоте снова штатный объект), чтобы IK не «плавал». Атомики клона на стандартном RW-пути (`renderCallBack` штатного оружия не копируется — иначе `RenderWeaponCB` падает без данных плагина). Альфа вспышки для материалов вроде `gunflash`, `muzzleflash`, `muzzle_texture*` синхронизируется с `m_nWeaponGunflashAlphaMP1`. Для выбора пресета замены в руках используется тип оружия в **активном слоте** без требования «есть патроны» на клиенте; если слот в фазе рендера «пустой» (часто в SA:MP), дополнительно берётся **`CPed::m_nWeaponModelId`** и обратный поиск по `CWeaponInfo` / `weapon.dat`, затем сопоставление слотов инвентаря с этой моделью.
- Опция **«Скрыть штатное оружие в руках, рисовать замену после»** / INI `[Features] WeaponReplacementHideBaseHeld=1`: на время `CPed::Render` в `m_pWeaponObject` подставляется `nullptr` (штатный клан не рисуется), кастомный клон дорисовывается в `pedRenderEvent.after` с **тем же** освещением, что оружие на теле: `ApplyAttachmentLightingForPed` (и при необходимости TXD на клоне), **без** `CPed::SetupLighting` / `RemoveLighting`, чтобы не ломать глобальные уличные/сценовые света. Поза клона берётся из той же секции оружия в OrcOutFit, что и для отображения на теле (кость `Bone`, смещения); если секция выключена или кость не задана, используется запасной узел **правая рука** (`Bone=24` в терминах RpHAnim node id в плагине).
- Если слот `m_pWeaponObject` в фазе рендера ped пуст (в частности при **выключенной** опции выше), подмена в руках выполняется дорисовыванием после внутреннего вызова в `CPed::Render` (поза от кости). Указатель на штатный клан для слота возвращается в конце кадра (после сцены), иначе оставшаяся часть `CPed::Render` снова нарисует ванильное оружие поверх замены.

### Текстуры оружия
- Подвкладка **Weapons → Textures** управляет отдельной TXD-подменой оружия без замены DFF-модели.
- Приоритет текстур: **`OrcOutFit\Weapons\Textures\Nick\<weapon>_<nick>.txd`** выше текстуры по скину; ник сравнивается без учёта регистра и без SA:MP color-кодов.
- Текстура по скину хранится в **`OrcOutFit\Weapons\Textures\<weapon>\<dff>.txd`**; random-варианты — в **`OrcOutFit\Weapons\Textures\<weapon>\<dff>\*.txd`**.
- Один ped получает один и тот же выбранный TXD-вариант для оружия на теле и видимого оружия в руках.

### Все педы в радиусе (опционально)
- Режимы **«оружие у всех ped»** и **«объекты у всех ped»** с радиусом от локального игрока.
- Для оружия используется отдельный per-ped cache инстансов; пресеты оружия подбираются по **`GetPedStdSkinDffName`**.
- Для объектов на каждом ped применяются секции по DFF стандартного ped; найденные секции и отсутствующие пары object/skin кешируются, чтобы режим не перечитывал INI каждый кадр.

### Кастомные объекты
- Сканирование **`*.dff`** в **`OrcOutFit\Objects`**.
- Для каждого объекта — **`<имя>.ini`**. Настройки по стандартным скинам ped — только в секциях **`[Skin.<dff_name>]`** (имя DFF из `LoadPedObject`, без отдельных папок «под скин»).
- Масштаб: `Scale` и `ScaleX/Y/Z`; условный рендер по списку оружия (`Weapons`, `WeaponsMode`, `HideWeapons`) внутри секции `[Skin.*]`.
- Подготовка материалов объектов выполняется один раз при загрузке инстанса; per-frame рендер больше не обходит все материалы объекта.

### Стандартные объекты игры
- Подвкладка **Objects → Standard objects** добавляет игровые модели по **model id**. Повтор одного ID поддерживается слотами вида **`123#1`**, **`123#2`**.
- Разрешены объектные типы моделей игры: atomic/time/weapon/clump/lod; ped и vehicle ID не принимаются.
- ID дополнительно проверяется на существование в текущей игре/SA:MP-сборке: если модель удалена или не имеет streaming/RW-источника, она не добавляется и не рендерится.
- Настройки крепления такие же, как у кастомных объектов: кость, offset, rotation, `Scale`, `ScaleX/Y/Z`, условие по оружию и `HideWeapons`.
- Стандартные объекты используют тот же lighting path для attachment-ов, что оружие и кастомные объекты, и отдельные runtime-инстансы по ped/slot.
- Список и настройки хранятся в **`OrcOutFit\Objects\StandardObjects.ini`**.

### Кастомные скины (DFF поверх педа)
- Сканирование **`*.dff`** в **`OrcOutFit\Skins`**.
- Рендер выбранного **clump** поверх ped, привязка анимации через **RpSkin** / иерархию базового ped.
- Цепочка освещения скина: **`CPed::SetupLighting`** (обёртка с SEH) → **`ApplyAttachmentLightingForPed`** с **`colourScale=1.0`** → **`RpClumpRender`** → при успешном **`SetupLighting`** — **`RemoveLighting`**. У оружия и кастом-объектов в той же функции освещения используется **`colourScale=0.5`**, чтобы уменьшить пересвет. Без отдельного **`PrepAtomicCB`** для скина.
- **Скрытие базового ped** (`CVisibilityPlugins::SetClumpAlpha`) — опция.
- **SA:MP** при включённом **`SkinNickMode`**: скин в основном **по нику** (`[NickBinding]` в INI скина); совпадение ника важнее выбранного в списке скина.
- Nick binding использует кеш `nick → skin`, а выбранный UI-скин кешируется по имени; кеш сбрасывается при рескане/сохранении и live-правках nick binding.
- **Локальный игрок:** **`SkinLocalPreferSelected`** (*Skin: always use selected skin for me*) включает выбранный в UI скин **в том числе при выключенном nick binding**; при включённом nick binding скин по совпадению ника по-прежнему **важнее**. Без этой опции и без ник-привязки оверлей на себя не ставится.

### Стандартные скины игры
- Подвкладка **Skins → Standard skins** использует список стандартных ped из кеша `LoadPedObject`, сортированный по model id: **`Имя [ID]`**.
- В список и примерку попадают только ped-модели, которые реально существуют в текущей игре/SA:MP-сборке.
- Доступны два сценария: overlay стандартного ped-clump поверх текущего ped и кнопка **Wear this skin (local player)**, которая меняет модель локального игрока через `CPed::SetModelIndex`.
- Выбранный overlay-источник хранится в `[SkinMode] SelectedSource=custom|standard`; стандартный выбор — в `[SkinMode] StandardSelected=<modelId>`.
- Nick binding стандартных скинов хранится в **`OrcOutFit\Skins\StandardSkins.ini`**. Приоритет выбора: кастомный skin по нику → стандартный skin по нику → выбранный локальный skin при `SkinLocalPreferSelected=1`.
- Стандартный overlay использует ту же цепочку освещения, что кастомный skin overlay: `SetupLighting` → `ApplyAttachmentLightingForPed` с `colourScale=1.0` → `RpClumpRender` → `RemoveLighting`.

### Предпросмотр скинов
- Подвкладка **Skins → Skin preview** показывает стандартные, кастомные и random-скины в отдельной D3D9 texture внутри ImGui.
- Preview создаётся перед основным RenderWare-рендером по подходу `EntityRender`: отдельная camera texture/z-buffer, затем восстановление основной камеры.
- Для custom/random preview используется тот же путь, что у overlay-скинов на ped: копирование позы по костям, `SetupLighting`, `ApplyAttachmentLightingForPed`, `RpClumpRender`, `RemoveLighting`.

### Рандомные скины
- Подвкладка **Skins → Random skins** включает random-пулы из **`OrcOutFit\Skins\Random\<dff>\*.dff`**.
- Папка `<dff>` должна совпадать с именем стандартного ped из `LoadPedObject`, например `swfyst`.
- Для ped с таким базовым DFF выбирается один случайный вариант из соответствующего пула; выбор закрепляется за ped и сбрасывается при смене модели, рескане или перезагрузке runtime-состояния.
- Random-пулы применяются после привязок по нику и после выбранного локального overlay-скина, если `SkinLocalPreferSelected=1`.
- Random-пулы могут работать отдельно от выбранного overlay-режима `SkinMode`: достаточно включить `RandomFromPools`.

### Texture remap стандартных ped
- Подвкладка **Skins → Texture** включает PedFuncs-style замену текстур стандартных ped-моделей.
- Если в TXD модели есть текстуры вида **`*_remap`**, плагин ищет базовую текстуру с именем до `_remap` и временно подменяет её в материалах ped перед рендером.
- Remap применяется не только к базовому ped clump, но и к standard/custom/random overlay-скинам; для random-скина сначала используется имя варианта, затем fallback на DFF папки пула.
- Для локального ped в UI доступны только реально найденные уникальные варианты, **Randomize local** и возврат к **Original textures**; PedFuncs-циклы по несуществующим номерам не используются.
- **Random mode**: `Linked variant` (по умолчанию) выбирает общий remap-index для всего ped, а если у отдельного слота такого индекса нет — берёт случайный реальный вариант этого слота. Повтор того же варианта подряд не запрещается.
- **Nick binding** для texture remap: текущий набор выбранных текстур можно сохранить для ника SA:MP. Binding имеет приоритет; если для ника binding не найден, используется random.
- **Auto texture by nickname**: при включённом `SkinTextureRemapAutoNickMode` плагин ищет в TXD текущего стандартного ped текстуры формата **`<original>_<nick>`** (`wmyclot_Walcher_Flett`, `body_Walcher_Flett`). Часть `<original>` выбирает слот базовой текстуры, а `<nick>` ищется внутри SA:MP-ника без учёта регистра и без цвет-кодов `{RRGGBB}`. Если для одного слота подходят несколько текстур, выбирается самое длинное совпадение. Auto-текстуры не попадают в random-пул.
- Приоритет texture remap по нику: ручной binding → auto texture by nickname → текущий random. Если auto-текстура найдена только для части слотов, остальные слоты продолжают использовать random.
- Полное имя texture name в RenderWare должно помещаться в **31 ASCII-символ**. Для длинных ников или длинных `<original>` используйте ручной binding через INI.
- Binding-файлы: **`OrcOutFit\Skins\Textures\<dff>.ini`**. Внутри сохраняются реальные имена original/remap-текстур, а не порядковые номера.
- Дополнительные `peds1..peds4.txd` кешируются через hook `AssignRemapTxd`, а поиск текстур расширяется hook-ом `RwTexDictionaryFindNamedTexture`, как в логике PedFuncs.

### Отладочный лог
- Файл **`OrcOutFit.log`** создаётся **рядом с `OrcOutFit.ini`** (не обязательно рядом с ASI при modloader — путь считается от INI).
- Уровни в **`[Features] DebugLogLevel`**: **`0`** — выкл.; **`1`** — только ошибки (префикс **`[E]`** в логе); **`2`** — полный trace (**`[I]`** + ошибки).
- Устаревший ключ **`DebugLog=1`** по-прежнему включает уровень **`2`** (если **`DebugLogLevel`** не задан).
- Настройка в UI: **Main** → **Debug log** (выпадающий список). Реализация: **`source/orc_log.cpp`**, **`source/orc_log.h`**.

### Интерфейс
- Вкладки **Main** (плагин, `[Features]`, пути), **Weapons**, **Objects**, **Skins**, **Settings**.
- Внутри **Weapons** три подвкладки: **Weapon render** (крепление оружия на теле), **Weapon replacement** (замена моделей оружия по скину/random/нику) и **Textures** (TXD-текстуры оружия по скину/random/нику).
- Внутри **Objects** две подвкладки: **Custom objects** (DFF из папки `Objects`) и **Standard objects** (модели игры по ID).
- Внутри **Skins** пять подвкладок: **Custom skins** (DFF поверх ped), **Standard skins** (ped-модели игры overlay / примерка), **Skin preview**, **Random skins** и **Texture** (`*_remap` для ped и overlay-скинов).
- Интерфейс локализован на русский и английский язык; выбор хранится в **`[Main] Language=ru|en`** и переключается во вкладке **Settings**.
- Кириллица в ImGui поддерживается через установленный шрифт Windows: основной **Arial**, fallback **Segoe UI / Tahoma / Calibri**. Путь к файлу берётся из реестра Windows Fonts (`HKLM` / `HKCU`) и системных каталогов Fonts, без жёсткой привязки к `C:\Windows\Fonts`; проект компилируется с **`/utf-8`**.
- Базовый шрифт ImGui по умолчанию — **15 px**. Во вкладке **Settings** можно менять **Auto-scale UI** (по умолчанию выключен), ручной **UI scale** и **Font size**; значения сохраняются в `[Main] UiAutoScale`, `UiScale`, `UiFontSize`.
- Стиль ImGui основан на `MyAsiModReshenie`: тёмная сине-серая палитра, более заметные рамки, мягкое скругление и голубой accent для активных элементов.
- Основное окно ImGui можно перетаскивать, но нельзя увести за пределы клиентской области игры: активный drag зажимается до `Begin()` через текущий ImGui `MovingWindow`, поэтому окно не мигает при попытке вывести его за границы; позиция и размер также зажимаются при первом показе, смене масштаба/разрешения и resize.
- Input/combo/checkbox/button-элементы в формах используют общий layout с правым запасом и шириной по внутренней области ячейки, чтобы контролы не уходили за правый край и под scrollbar.
- Во вкладке **Settings** рядом с полем **ActivationKey** есть серый маркер **`(?)`** с подсказкой по допустимым значениям клавиши.
- Список стандартных ped для редактирования/примерки: **сортировка по model id по возрастанию**, подписи **`Имя [ID]`**.
- **Wear this skin (local player)** находится в **Skins → Standard skins** и меняет модель локального педа для превью: стриминг (`RequestModel` / `LoadAllRequestedModels`), затем `CPed::SetModelIndex` в начале следующего кадра (безопасно для clump).
- В **Weapons → Weapon render** оставлены только две кнопки сохранения:
  - **Save to Global (`OrcOutFit.ini`)** — сохраняет весь набор оружия (primary + secondary) в глобальный INI.
  - **Save to skin (`OrcOutFit\Weapons`)** — сохраняет весь набор оружия (primary + secondary) в `Weapons\<dff>.ini`.
- Сохранение INI выполняется пакетно: плагин собирает изменения в памяти и пишет файл одним проходом, чтобы не стопорить игру серией WinAPI INI-записей.
- **Save to skin (`OrcOutFit\Weapons`)** недоступен только в **одиночной игре** для **дефолтного CJ** (`MODEL_PLAYER`); в SA:MP и после примерки другой модели сохранение доступно.
- В редакторе оружия: **Copy/Paste** у `Show on body`.
- В редакторах **Weapons** и **Objects** включён **live preview**: offset/rotation/scale применяются сразу в рендере без обязательного сохранения.

### Кратко по новым функциям
- **Стандартный объект:** откройте **Objects → Standard objects**, введите **model ID**, нажмите **Add to render list**, выберите слот `ID#N`, выберите ped skin `Имя [ID]`, настройте кость/позицию/поворот/scale/фильтр оружия и сохраните.
- **Дубликаты объекта:** добавьте тот же ID повторно — появится следующий слот (`123#1`, `123#2`), каждый слот настраивается отдельно.
- **Стандартный skin overlay:** откройте **Skins → Standard skins**, выберите ped model, в **Selected overlay skin source** выберите стандартный skin. Для локального игрока без ника включите **SkinLocalPreferSelected**.
- **Примерка стандартного skin:** в **Skins → Standard skins** нажмите **Wear this skin (local player)** — модель локального игрока меняется через `CPed::SetModelIndex`.
- **Предпросмотр skin:** откройте **Skins → Skin preview**, выберите источник Standard/Custom/Random и нужный вариант.
- **Random skin pool:** положите варианты в `OrcOutFit\Skins\Random\<dff>\`, откройте **Skins → Random skins**, включите **Enable random skins** и нажмите **Rescan skins**.
- **Замена оружия:** положите замену по скину в `OrcOutFit\Weapons\Guns\<weapon>\<dff>.dff`, random-варианты в `OrcOutFit\Weapons\Guns\<weapon>\<dff>\*.dff`, а замену по нику в `OrcOutFit\Weapons\GunsNick\<weapon>_<nick>.dff`; настройки находятся в **Weapons → Weapon replacement**.
- **Текстуры оружия:** положите TXD по скину в `OrcOutFit\Weapons\Textures\<weapon>\<dff>.txd`, random-варианты в `OrcOutFit\Weapons\Textures\<weapon>\<dff>\*.txd`, а TXD по нику в `OrcOutFit\Weapons\Textures\Nick\<weapon>_<nick>.txd`; настройки находятся в **Weapons → Textures**.
- **Привязка по нику:** в **Standard skins** включите nick binding, укажите ники через запятую и сохраните INI.

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
   - `OrcOutFit\Objects\StandardObjects.ini` — список и настройки стандартных игровых объектов по ID
   - `OrcOutFit\Weapons` — опциональные пресеты оружия `<dff>.ini`
   - `OrcOutFit\Weapons\Guns` — замены моделей оружия по скину и random-пулы оружия
   - `OrcOutFit\Weapons\GunsNick` — замены моделей оружия по нику SA:MP
   - `OrcOutFit\Weapons\Textures` — TXD-текстуры оружия по скину/random/нику
   - `OrcOutFit\Skins` — кастомные скины DFF/TXD
   - `OrcOutFit\Skins\Random` — random-пулы скинов по базовому DFF ped
   - `OrcOutFit\Skins\StandardSkins.ini` — nick binding стандартных скинов
   - `OrcOutFit\Skins\Textures` — nick bindings для texture remap `<dff>.ini`

---

## Сборка

- Visual Studio + **MSBuild**, платформа решения: **`x86`** (в `.vcxproj` — Win32).
- Локальный проект использует `PlatformToolset=v145` (Visual Studio 18). В GitHub Actions сборка намеренно переопределяется на `PlatformToolset=v143`, потому что `windows-latest` предоставляет v143 build tools.

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" OrcOutFit.sln /p:Configuration=Release /p:Platform=x86
```

Артефакт: **`build\Release\OrcOutFit.asi`** (каталог `build/` в `.gitignore`; после сборки появляется локально).

Зависимости: **plugin-sdk**, **imgui**, **MinHook** в `source\external\`.

### Кодировка исходников (UTF-8)

- Файлы **`source\*.cpp`** и **`source\*.h`**, кроме vendored **`source\external\`**, храните в **UTF-8** (в Visual Studio: «Сохранить с кодировкой…» → UTF-8; в VS Code: кодировка UTF-8). Проект компилируется с **`/utf-8`**. Сохранение в **Windows-1251** или другой **ANSI**-странице портит русские комментарии (двойная «перекодировка»).
- Пользовательский текст интерфейса — только через **`source/orc_locale.cpp`**, а не новыми строками в произвольных модулях.
- Скрипт ручного восстановления: **`tools/fix_mojibake.py`** (нужен Python 3 и пакет `ftfy`: `py -3 -m pip install ftfy`).

### GitHub Actions (Release Win32)

- Workflow: **`.github/workflows/build-release-win32.yml`**.
- Триггеры: ручной запуск (`workflow_dispatch`) и публикация релиза (`release.published`).
- Имя workflow run в списке Actions: для **release** — название релиза или тег; для **workflow_dispatch** и прочих событий — **`ветка (#номер_run)`** (например `main (#42)`), чтобы не отображался только короткий SHA.
- Сборка в CI: сначала `source/external/plugin-sdk/plugin_sa/Plugin_SA.vcxproj` (`Release|Win32`, `PlatformToolset=v143`) для `Plugin.lib`, затем `OrcOutFit.sln` (`Release|x86`, `PlatformToolset=v143`). Это намеренно отличается от локального `v145`.
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
| Одиночная игра | **`[Main] ActivationKey`** (по умолчанию **F7**), настройка во вкладке **Settings** |
| SA:MP | **`[Main] Command`** (по умолчанию **`/orcoutfit`**), настройка во вкладке **Settings**. Опционально **`SampAllowActivationKey=1`** |

Удержание **ПКМ** отдаёт камере игры (меню может оставаться открытым).
ImGui-рендер подключается через D3D9 hooks (`Present` / fallback `EndScene`, `Reset`) после первого drawable-кадра игры, поэтому одиночная игра не ждёт полной инициализации SA:MP. Для стабильности курсора/drag используется sticky-capture; в распознанном SA:MP периодически подтверждается `SetCursorMode`, а в одиночной игре и неизвестных сборках работает single-player fallback. `d3dLost`/`d3dReset` очищают render target предпросмотра и временные remap/weapon texture overrides. При сбое рендера меню курсор и управление принудительно возвращаются игре.

---

## `OrcOutFit.ini`

### `[Main]`
| Ключ | Описание |
|------|----------|
| `Enabled` | Плагин вкл/выкл |
| `Language` | Язык интерфейса: `ru` или `en` |
| `ActivationKey` | Клавиша (SP и опционально SA:MP) |
| `SampAllowActivationKey` | В SA:MP также разрешить клавишу (`0`/`1`) |
| `Command` | Чат-команда (с `/`) |
| `UiAutoScale` | `1` — автоматически масштабировать отступы, окно и контролы под текущее разрешение; `0` — только ручной `UiScale` (по умолчанию) |
| `UiScale` | Ручной множитель масштаба интерфейса (`0.75..1.60`) |
| `UiFontSize` | Базовый размер шрифта ImGui (`13..22`, по умолчанию `15`) |

### `[Features]`
| Ключ | Описание |
|------|----------|
| `RenderAllPedsWeapons` | Оружие у всех ped в радиусе |
| `RenderAllPedsObjects` | Кастомные и стандартные объекты у всех ped в радиусе |
| `RenderAllPedsRadius` | Радиус (метры) |
| `ConsiderWeaponSkills` | Dual wield по навыку |
| `CustomObjects` | Рендер объектов из `Objects` |
| `StandardObjects` | Рендер стандартных игровых объектов из `Objects\StandardObjects.ini` |
| `WeaponReplacement` | Включить замену моделей оружия |
| `WeaponReplacementOnBody` | Применять замену к оружию, которое OrcOutFit рисует на теле |
| `WeaponReplacementInHands` | Применять замену к реальному видимому оружию в руках ped |
| `WeaponTextures` | Включить TXD-текстуры оружия |
| `WeaponTextureNickMode` | Включить приоритетные TXD-текстуры оружия по нику SA:MP |
| `WeaponTextureRandomMode` | Включить random-пулы TXD-текстур оружия |
| `SkinMode` | Режим overlay-скинов |
| `SkinHideBasePed` | Скрыть базовый clump |
| `SkinNickMode` | Привязка скинов по нику SA:MP |
| `SkinLocalPreferSelected` | `1` — всегда использовать **выбранный в UI** скин на локальном игроке (если нет скина по нику); `0` — только ник |
| `SkinTextureRemap` | Включить PedFuncs-style texture remap для стандартных ped TXD (`*_remap`) |
| `SkinTextureRemapNickMode` | Включить приоритетные texture-remap привязки по нику SA:MP |
| `SkinTextureRemapAutoNickMode` | Включить автоматические texture-remap привязки по текстурам `<original>_<nick>` в TXD текущего стандартного ped |
| `SkinTextureRemapRandomMode` | `0` = `Per texture`, `1` = `Linked variant` (дефолт) |
| `DebugLogLevel` | `0` / `1` (ошибки) / `2` (info+ошибки); см. раздел «Отладочный лог» |
| `DebugLog` | Legacy: `1` = то же, что **`DebugLogLevel=2`**, если уровень не задан отдельно |

### Секции оружия (корень INI)
Как раньше: именованные (`[M4]`) и `[WeaponNN]`, для второго ствола — `[WeaponNN_2]` / `[Name2]`.

### `[SkinMode]`
| Ключ | Описание |
|------|----------|
| `Selected` | Имя выбранного кастомного скина (basename `.dff`) |
| `SelectedSource` | `custom` или `standard` — источник выбранного overlay-скина для локального игрока |
| `StandardSelected` | Model id выбранного стандартного скина |
| `RandomFromPools` | `1` — включить random-пулы `OrcOutFit\Skins\Random\<dff>\*.dff`; `0` — выключить |

---

## INI объекта (`OrcOutFit\Objects\<имя>.ini`)

Секции **`[Skin.<dff_name>]`** (имя как в ped.dat / кеше `LoadPedObject`): `Enabled`, кость, offset, rotation, scale, `ScaleX/Y/Z`, опционально `Weapons` / `WeaponsMode` / `HideWeapons`.

## INI стандартных объектов (`OrcOutFit\Objects\StandardObjects.ini`)

`[Objects] Entries=123#1,123#2,...` хранит список добавленных игровых моделей. Настройки по ped skin хранятся в секциях **`[Object.<modelId>#<slot>.Skin.<dff_name>]`** с теми же ключами, что у кастомных объектов.

---

## Файлы замены оружия

- `OrcOutFit\Weapons\GunsNick\<weapon>_<nick>.dff/.txd` — замена по нику SA:MP, самый высокий приоритет.
- `OrcOutFit\Weapons\Guns\<weapon>\<dff>.dff/.txd` — уникальная замена оружия для стандартного ped DFF.
- `OrcOutFit\Weapons\Guns\<weapon>\<dff>\*.dff/.txd` — random-варианты для такого же `<weapon>` и `<dff>`.

Если TXD рядом с DFF не найден, модель не загружается и плагин использует обычную игровую модель.

---

## Файлы текстур оружия

- `OrcOutFit\Weapons\Textures\Nick\<weapon>_<nick>.txd` — TXD по нику SA:MP, самый высокий приоритет.
- `OrcOutFit\Weapons\Textures\<weapon>\<dff>.txd` — уникальная TXD-текстура оружия для стандартного ped DFF.
- `OrcOutFit\Weapons\Textures\<weapon>\<dff>\*.txd` — random-варианты TXD для такого же `<weapon>` и `<dff>`.

TXD должен содержать текстуры с теми же именами, что используются материалами текущей модели оружия.

---

## INI кастомного скина (`OrcOutFit\Skins\<имя>.ini`)

**`[NickBinding]`**: `Enabled`, `Nicks` (через запятую и/или новую строку, без учёта регистра).

## INI стандартных скинов (`OrcOutFit\Skins\StandardSkins.ini`)

`[StandardSkins] Entries=<dff1>,<dff2>,...`; для каждого DFF секция **`[Skin.<dff>]`** хранит `ModelId`, `Enabled`, `Nicks`.

## INI texture binding (`OrcOutFit\Skins\Textures\<dff>.ini`)

Секции **`[Binding.N]`**: `Enabled`, `Nicks`, `SlotCount`, пары `SlotNOriginal` / `SlotNRemap`. Новые binding-и имеют больший `N` и при совпадении ника перекрывают старые.

---

## Поддержка SA:MP

Детект по entry point `samp.dll`; для известных клиентов — хук команды, ники, `SetCursorMode`.

---

## Структура исходников

| Путь | Назначение |
|------|------------|
| `source/main.cpp` | Точка входа, конфиг, weapon.dat / ped.dat хуки, `SyncAndRender`, сохранение INI |
| `source/orc_app.h`, `source/orc_types.h` | Состояние плагина, типы, объявления модулей |
| `source/orc_weapons.cpp`, `source/orc_weapons.h` | Хук `LoadWeaponObject`, weapon.dat |
| `source/orc_weapon_runtime.cpp`, `source/orc_weapon_runtime.h` | Оружие на теле, held replacement, `OrcSyncPedWeapons` / `OrcRenderPedWeapons` |
| `source/orc_weapon_assets.cpp`, `source/orc_weapon_assets.h` | Скан Guns/GunsNick/Textures, замена DFF/TXD |
| `source/orc_path.cpp`, `source/orc_path.h` | Пути, `OrcToLowerAscii`, поиск TXD |
| `source/orc_attach.cpp`, `source/orc_attach.h` | Подготовка atomic/clump для вложений |
| `source/orc_render.cpp`, `source/orc_render.h` | Кости, смещение матрицы, `OrcApplyAttachmentLightingForPed` |
| `source/orc_objects.cpp` | Кастомные/стандартные объекты, `Objects\`, `StandardObjects.ini`, рендер вложений |
| `source/orc_skins.cpp` | Кастомные/стандартные/random скины, превью, overlay |
| `source/orc_weapons_ui.cpp` | ImGui-вкладка Weapons |
| `source/orc_ui_bones.cpp`, `source/orc_ui_bones.h` | Список костей для UI |
| `source/orc_texture_remap.cpp`, `source/orc_texture_remap.h` | Texture remap стандартных ped TXD (`*_remap`) |
| `source/orc_locale.cpp`, `source/orc_locale.h` | Ключи и строки локализации интерфейса (`ru` / `en`) |
| `source/orc_log.cpp`, `source/orc_log.h` | Лог в файл, уровни Info/Error |
| `source/orc_ui.cpp` | ImGui (кроме вынесенного UI оружия) |
| `source/overlay.cpp` | D3D9 hooks + ввод/курсор |
| `source/samp_bridge.cpp` | SA:MP |
| `tools/fix_mojibake.py` | Опционально: исправление случайно испорченной UTF-8 в исходниках (см. «Кодировка исходников») |

---

## Ограничения

- Ориентация на **GTA SA 1.0 US** (адреса в коде).
- Не копируйте ASI в игру, пока процесс запущен и держит файл.

---

## Автор

**[@dmitriyewich](https://github.com/dmitriyewich)** — исходники и проект VS: [OrcOutFit](https://github.com/dmitriyewich/OrcOutFit).
