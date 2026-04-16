[size=180][b]OrcOutFit[/b][/size]

Нативный [b]ASI-плагин[/b] для [b]GTA San Andreas 1.0 US[/b] (x86), в том числе для [b]SA:MP[/b].
Рисует оружие на теле педа, кастомные объекты и кастомные скины, настраивается через ImGui-оверлей и INI-файлы.

[hr]

[size=140][b]Возможности[/b][/size]

[b]Оружие на теле[/b]
- Отображение моделей оружия на выбранной кости (RpHAnim node ID) со смещением/поворотом/масштабом.
- Оружие в активном слоте (в руках) на теле не дублируется.
- Секции по имени оружия (например [code]M4[/code]) и резервные секции [code][WeaponNN][/code] для кастомных типов.

[b]Все педы в радиусе (опционально)[/b]
- Режим «оружие у всех педов» с ограничением по радиусу от локального игрока.
- Отдельный кэш инстансов на каждого педа.

[b]Кастомные объекты[/b]
- Сканирование [code]*.dff[/code] в папке [code]OrcOutFit\object[/code]
- Для каждого объекта — свой [code]<имя>.ini[/code]: кость, смещение, поворот, масштаб, вкл/выкл.

[b]Кастомные объекты по стандартной модели (скину) ped[/b]
- Папка [code]OrcOutFit\object\other\<skin>\[/code содержит объекты и настройки для конкретной стандартной модели.
- <skin> может быть именем модели (например [code]wmyclot[/code]) или [code]id217[/code]/[code]217[/code] (по model id).

[b]Переопределение оружия по стандартной модели[/b]
- [code]OrcOutFit\object\other\<skin>\weapons.ini[/code — отдельные offsets/rot/scale под конкретный стандартный скин.
- Если [code]weapons.ini[/code отсутствует — используются глобальные значения из [code]OrcOutFit.ini[/code].

[b]Скины[/b]
- Сканирование [code]*.dff[/code] в [code]OrcOutFit\SKINS[/code.
- Рендер выбранного clump поверх педа, привязка анимации через RpSkin.
- Скрытие базового педа (альфа clump) — опция.
- SA:MP: привязка по нику (в INI скина список ников).

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

