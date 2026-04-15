# Context

## Назначение проекта

- `WeaponsOutFit` это нативный `ASI`-плагин для `GTA San Andreas / SA:MP`.
- Плагин дорисовывает на теле локального игрока оружие из инвентаря:
  - на спине
  - на левом бедре
  - на правом бедре
- Оружие, которое сейчас находится в руках, на теле не рисуется.
- Выходной артефакт проекта:
  - `WeaponsOutFit.asi`

## Целевая платформа

- GTA San Andreas `1.0 US`.
- Только `Win32` (x86).

## Текущая рабочая логика

- Плагин ждёт, пока игра догрузится (`kLoadState == 9`).
- Ставится хук на целевую функцию `drawingEvent` (подход из `BaseModelRender`):
  - call-site: `0x53E293` (`E8 rel32`)
  - целевой адрес читается из `rel32` на старте и хукается через `MinHook`.
- В хуке после вызова оригинала:
  - берётся `FindPlayerPed(0)`
  - синхронизируется список оружия в инвентаре
  - для каждого `мапленного` оружия (не текущего) создаётся инстанс модели
  - инстанс рендерится на нужной кости через `RpHAnim`-матрицу

## Важные файлы

- Основной код:
  - `C:\Games\CODEX\WeaponsOutFit\source\main.cpp`
- MinHook:
  - `C:\Games\CODEX\WeaponsOutFit\source\external\MinHook\`
- Проект Visual Studio:
  - `C:\Games\CODEX\WeaponsOutFit\WeaponsOutFit.vcxproj`
- Solution:
  - `C:\Games\CODEX\WeaponsOutFit\WeaponsOutFit.sln`
- Конфиг:
  - `C:\Games\CODEX\WeaponsOutFit\WeaponsOutFit.ini`
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
  - `Release | Win32`
- Ожидаемый выходной файл:
  - `C:\Games\CODEX\WeaponsOutFit\build\Release\WeaponsOutFit.asi`
- **НЕЛЬЗЯ** автоматически копировать / перемещать `.asi` куда-либо.
  - Единственное допустимое расположение — `build\Release\WeaponsOutFit.asi`.
  - Деплой в папку игры (`C:\Games\SAMP\GTA San Andreas\`) делает пользователь вручную.
  - Причина: `.asi` в игре бывает залочен запущенным процессом, а двойная копия рассинхронизируется.

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
