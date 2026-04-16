[size=180][b]OrcOutFit[/b][/size]

Нативный [b]ASI-плагин[/b] для [b]GTA San Andreas 1.0 US[/b] (x86), в том числе для [b]SA:MP[/b].
Рисует оружие на теле педа, кастомные объекты и кастомные скины, настраивается через ImGui-оверлей и INI-файлы.

[hr]

[size=140][b]Возможности[/b][/size]

[b]Оружие на теле[/b]
- Отображение моделей оружия на выбранной кости (RpHAnim node ID) со смещением/поворотом/масштабом.
- Оружие в активном слоте (в руках) на теле не дублируется.
- Секции по имени оружия (например [code]M4[/code]) и резервные секции [code][WeaponNN][/code] для кастомных типов.
- Dual wield (skills): при прокачке навыка (PRO) и поддержке типа оружия игра может выдавать второй ствол (например для [code]Pistol [22][/code]); плагин умеет рендерить и настраивать вторую копию.
  - Проверка [code]bTwinPistol[/code]: [code]GetWeaponInfo(wt,2)[/code] (fallback [code]1[/code]), а [code]WEAPSKILL_PRO[/code] проверяется у конкретного ped (локальный и all-peds).
- Авто-скан оружия: основной источник — хук [code]CFileLoader::LoadWeaponObject (0x5B3FB0)[/code] через MinHook ([code]weapon.dat -> wt/modelId[/code]), плюс fallback на CWeaponInfo. Формат в UI: [code]Name [weaponTypeId][modelId][/code].
- Авто-загрузка модели оружия: если модель ещё не загружена, плагин запрашивает стриминг и начинает рендерить со следующего кадра.

[b]Все педы в радиусе (опционально)[/b]
- Режим «оружие у всех педов» с ограничением по радиусу от локального игрока.
- Отдельный кэш инстансов на каждого педа.

[b]Кастомные объекты[/b]
- Сканирование [code]*.dff[/code] в папке [code]OrcOutFit\object[/code]
- Для каждого объекта — свой [code]<имя>.ini[/code]: кость, смещение, поворот, масштаб, вкл/выкл.
- Масштаб: общий [code]Scale[/code] и дополнительные множители [code]ScaleX[/code]/[code]ScaleY[/code]/[code]ScaleZ[/code] по осям.
- Можно задать условие «рендерить объект только при наличии выбранного оружия» (и опционально скрывать это оружие на теле).

[b]Кастомные объекты по стандартной модели (скину) ped[/b]
- Папка [code]OrcOutFit\object\other\<skin>\[/code содержит объекты и настройки для конкретной стандартной модели.
- <skin> может быть именем модели (например [code]wmyclot[/code]) или [code]id217[/code]/[code]217[/code] (по model id).
- Для определения имени/ID модели используется также хук [code]CFileLoader::LoadPedObject (0x5B7420)[/code]; приоритет имени модели выше [code]id###[/code] fallback.

[b]Переопределение оружия по стандартной модели[/b]
- [code]OrcOutFit\object\other\<skin>\weapons.ini[/code — отдельные offsets/rot/scale под конкретный стандартный скин.
- Если [code]weapons.ini[/code отсутствует — используются глобальные значения из [code]OrcOutFit.ini[/code].

[b]UI оружия[/b]
- Рядом с [code]Show on body[/code] есть [code]Copy[/code]/[code]Paste[/code] (Global/Local skin/Other skin).
- Paste делает валидацию буфера; вставка разрешена и между primary/secondary (dual wield).

[b]Скины[/b]
- Сканирование [code]*.dff[/code] в [code]OrcOutFit\SKINS[/code.
- Рендер выбранного clump поверх педа, привязка анимации через RpSkin.
- Скрытие базового педа (альфа clump) — опция.
- SA:MP: привязка по нику (в INI скина список ников).
- Для [code]SKINS\random\<folder>[/code] резолв модели: [code]CModelInfo[/code] по имени -> имя из кеша [code]LoadPedObject[/code] -> [code]id###/###[/code] fallback.

[hr]

[size=140][b]Установка[/b][/size]

1) Поместите [code]OrcOutFit.asi[/code] в папку игры (или используйте modloader — пути данных считаются от папки с ASI).
2) Рядом с ASI появится [code]OrcOutFit.ini[/code].
3) Создайте папки (относительно ASI), если нужно:
- [code]OrcOutFit\object[/code
- [code]OrcOutFit\object\other\<skin>\[/code
- [code]OrcOutFit\SKINS[/code
- [code]OrcOutFit\SKINS\random\<model_name>\[/code (опционально)

[hr]

[size=140][b]Открытие меню[/b][/size]

[b]Singleplayer:[/b] клавиша из [code]OrcOutFit.ini[/code] → [code][Main][/code] → [code]ActivationKey[/code (по умолчанию F7)
[b]SA:MP:[/b] команда из [code]Command[/code (по умолчанию [code]/orcoutfit[/code). Можно включить [code]SampAllowActivationKey=1[/code.

При удержании ПКМ — управление камерой отдаётся игре (меню остаётся открытым).

[hr]

[size=140][b]Сборка[/b][/size]

MSBuild (платформа решения: [b]x86[/b]):
[code]"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" OrcOutFit.sln /p:Configuration=Release /p:Platform=x86[/code]

Артефакт: [code]build\Release\OrcOutFit.asi[/code

[hr]

[size=140][b]Plugin.lib (plugin-sdk)[/b][/size]

OrcOutFit линкуется с [code]Plugin.lib[/code] из vendored plugin-sdk:
- путь: [code]source\external\plugin-sdk\output\lib\Plugin.lib[/code
- файл может отсутствовать после клона/обновления (это артефакт)

Если сборка падает с ошибкой про [code]Plugin.lib[/code:
1) Сгенерируйте проекты plugin-sdk:
[code]cd source\external\plugin-sdk\tools\premake
premake5.exe --pluginsdkdir="C:\Games\CODEX\WeaponsOutFit\source\external\plugin-sdk" vs2022[/code]
2) Соберите [code]Plugin_SA[/code] (Release|Win32) и убедитесь, что появился [code]output\lib\Plugin.lib[/code.
Если toolset отличается (например нужен v145), поправьте [code]<PlatformToolset>[/code] в [code]Plugin_SA\Plugin_SA.vcxproj[/code.
 
[hr]

[size=140][b]OrcOutFit.ini[/b][/size]

[b]Секция [code][Main][/code][/b]
- [code]Enabled[/code] — 0/1
- [code]RenderAllPedsWeapons[/code] — 0/1
- [code]RenderAllPedsRadius[/code] — радиус в метрах
- [code]ConsiderWeaponSkills[/code] — 0/1 (dual wield / второй ствол)
- [code]ActivationKey[/code] — клавиша (SP)
- [code]SampAllowActivationKey[/code] — 0/1
- [code]Command[/code] — чат-команда (SA:MP)

[b]Секции оружия[/b]
- Именованные: [code][M4][/code], [code][Pistol][/code], ...
- Числовые: [code][WeaponNN][/code]
- Для второго ствола (dual wield): [code][Pistol2][/code] или [code][Weapon22_2][/code] (аналогичные поля)

[hr]

[size=140][b]INI кастомного объекта[/b][/size]

Файл: [code]OrcOutFit\object\<имя>.ini[/code (и аналогично внутри [code]object\other\<skin>\[/code)

[b]Секция [code][Main][/code][/b]
- Базовые поля: [code]Enabled[/code], [code]Bone[/code], [code]OffsetX/Y/Z[/code], [code]RotationX/Y/Z[/code], [code]Scale[/code], [code]ScaleX/Y/Z[/code]
- Условие по оружию (опционально):
  - [code]Weapons[/code] = CSV weapon ids, например [code]22,23[/code
  - [code]WeaponsMode[/code] = [code]any[/code] или [code]all[/code
  - [code]HideWeapons[/code] = 0/1 (скрывать выбранное оружие на теле, когда объект рендерится)

[hr]

[size=140][b]INI кастомного скина[/b][/size]

Файл: [code]OrcOutFit\SKINS\<имя>.ini[/code (или в [code]SKINS\random\...[/code)

[b]Секция [code][NickBinding][/code][/b]
- [code]Enabled[/code] — 0/1
- [code]Nicks[/code] — только через запятую, например [code]Testovik,Walcher_Flett,OtherNick[/code (без учёта регистра)