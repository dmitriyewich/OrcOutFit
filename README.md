# OrcOutFit

<img width="238" height="408" alt="kHKmcxq" src="https://github.com/user-attachments/assets/44e51b41-9600-4cb8-ae4e-611afc44ed08" />

Нативный **ASI-плагин** для **GTA San Andreas 1.0 US** (x86), включая **SA:MP**.  
Полная документация и инструкции перенесены в wiki: [OrcOutFit Wiki](https://github.com/dmitriyewich/OrcOutFit/wiki).

---

## Возможности

- Отображение оружия на теле педа с настройкой позиции, поворота и масштаба.
- Коррекция позы оружия в руках (Held) через `OrcOutFit\Weapons\<dff>.ini`. Вспышка следует за стволом и использует таймер своей руки. Если в кастомном DFF нет геометрии `gunflash`, плагин привязывает к дульному срезу доступную штатную вспышку. Положение дыма, света и гильз учитывает позу оружия.
- Замена моделей и текстур оружия (включая сценарии для SA:MP); папка оружия берётся по DFF-имени из `LoadWeaponObject` / IDE (`desert_eagle`, `chromegun`, `mp5lng` и т.п.), random-пулы DFF/TXD могут выбираться случайно, последовательно по кругу или случайно без повтора среди живых ped. Ручной рескан замены перечитывает `Guns` / `GunsNick` и заново выбирает random-варианты для уже известных ped. При **PRO** и twin-пистолетах (`colt45` и аналоги) замена в руках работает для **обеих** рук — см. wiki «Оружие» → «Замена оружия».
  Если у CJ в одиночной игре нет базового DFF-имени скина, модель выбирается из общего пула `Weapons\Guns\<weapon>\`.
- Иконка оружия в штатном HUD локального игрока из выбранного TXD (`[Features] WeaponHudIconFromGunsTxd=1`). Подробности — в wiki «Оружие».
- Кастомные звуки замены в форматах **.wav / .mp3 / .flac / .ogg**. Включаются на вкладке **Оружие → Звуки** или через `[Features] CustomWeaponSounds=1`. Локальные одиночные звуки звучат в 2D, звуки удалённых стрелков и циклы — в 3D с отсечением по `MaxDist` от камеры. Если рабочий `_distant` отсутствует, используется обычный кастомный выстрел того же DFF с 3D-затуханием. При отсутствии рабочего сэмпла остаётся штатный звук. Затухание и реверб OpenAL EFX настраиваются в `[WeaponAudio]` и в `<stem>.audio` рядом с DFF. Встроенный OpenAL Soft учитывает громкость SFX GTA.
- Кастомные и стандартные объекты с привязкой к скинам.
- Кастомные, стандартные и random-скины. Пулы вариантов поддерживают случайную, последовательную и случайную выдачу без повтора. `SkinRandomIncludeVanilla=1` добавляет исходного педа в пул. **Native replacement** по умолчанию сохраняет игровой ID педа. Для других педов и random-скинов native-замена применяется при создании или смене модели. **Overlay** сохраняет прозрачность штатного рендера и использует базовый скин при несовместимом скелете. Random-пулы привязаны к базовому имени DFF. Их можно использовать и с выключенным ручным выбором скинов.
- Texture remap для стандартных и заменённых скинов. Поддерживаются общие словари PedFuncs `peds1.txd` … `peds5.txd` и подстановка из необязательного `GangHands.txd` при жестах. См. wiki «Скины» → «Текстуры».
- Настройка через ImGui-меню и INI-файлы.

Подробнее по вкладкам и подвкладкам:
- [Home (описание проекта)](https://github.com/dmitriyewich/OrcOutFit/wiki)
- [Главная](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%93%D0%BB%D0%B0%D0%B2%D0%BD%D0%B0%D1%8F)
- [Оружие](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%9E%D1%80%D1%83%D0%B6%D0%B8%D0%B5)
- [Объекты](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%9E%D0%B1%D1%8A%D0%B5%D0%BA%D1%82%D1%8B)
- [Скины](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%A1%D0%BA%D0%B8%D0%BD%D1%8B)
- [Настройки](https://github.com/dmitriyewich/OrcOutFit/wiki/%D0%9D%D0%B0%D1%81%D1%82%D1%80%D0%BE%D0%B9%D0%BA%D0%B8)

---

## Требования и ограничения

| Параметр | Значение |
|----------|----------|
| Игра | GTA San Andreas **1.0 US** |
| Архитектура | **Win32 (x86)** |
| SA:MP | Поддерживаемые `samp.dll`: **R1, R2, R3, R3-1, R4, R4-2, R5-1, DL-R1** |
| Ограничение | Для клиентов вне списка SA:MP-часть может работать частично или в fallback-режиме |
| Ограничение | Не заменяйте ASI-файл во время работы игры |

---

## Установка

1. Получите `OrcOutFit.asi` — готовый релиз или локальная сборка.
2. Поместите `OrcOutFit.asi` в каталог игры (или в modloader).
3. Запустите игру. Рядом с ASI появится или будет прочитан `OrcOutFit.ini`.
4. Откройте меню клавишей `F7` в одиночной игре или командой `/orcoutfit` в SA:MP. Для `F7` в SA:MP включите `[Main] SampAllowActivationKey=1`.
5. При необходимости задайте уровень лога в `[Features] DebugLogLevel`: `0` — в `OrcOutFit.log` ничего не пишется (включая ошибки), `1` — только ошибки, `2` — полный trace. При каждой новой игровой сессии лог рядом с INI пересоздаётся; при уровне `0` он остаётся пустым.

При переходе с Lite уберите `OrcOutFitLite.asi` из каталога игры или ModLoader. Существующий `OrcOutFit.ini` можно использовать с `OrcOutFit.asi`.

## Сборка из исходников

Нужны Visual Studio 18 с инструментами C++ x86 (`v145`) и CMake. Выполните из корня репозитория в PowerShell.

```powershell
git submodule update --init --recursive
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$env:PLUGIN_SDK_DIR = (Resolve-Path 'source/external/plugin-sdk').Path
& $msbuild source/external/plugin-sdk/plugin_sa/Plugin_SA.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
& $msbuild OrcOutFit.sln /t:Build /p:Configuration=Release /p:Platform=x86
```

PreBuild собирает статический OpenAL Soft 1.24.3. Плагин загружается без отдельной `OpenAL32.dll`. Артефакт — `build/Release/OrcOutFit.asi`. Локальная сборка также создаёт PDB. GitHub Actions использует `v143` и собирает ASI без PDB.

---

## Автор

**[@dmitriyewich](https://github.com/dmitriyewich)** — [OrcOutFit](https://github.com/dmitriyewich/OrcOutFit)
