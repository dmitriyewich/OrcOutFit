[size=180][b]OrcOutFit[/b][/size]

Нативный [b]ASI-плагин[/b] для [b]GTA San Andreas 1.0 US[/b] (x86), в том числе для [b]SA:MP[/b].
Рисует оружие на теле педа, кастомные объекты и кастомные скины; настройка через ImGui и INI.

[b]Репозиторий:[/b] [url=https://github.com/dmitriyewich/OrcOutFit]github.com/dmitriyewich/OrcOutFit[/url] — исходники, [b].sln/.vcxproj[/b], [code]README.md[/code], [code]README.txt[/code], [code]context.md[/code].

[hr]

[size=140][b]Возможности (кратко)[/b][/size]

[b]Оружие на теле[/b] — кость RpHAnim, offset/rot/scale; активный слот не дублируется; [code][WeaponNN][/code]; dual wield (второй ствол: [code][WeaponNN_2][/code] / [code][Name2][/code]).
Хук [code]LoadWeaponObject[/code] (MinHook) + fallback; авто-стриминг модели оружия.

[b]Пресеты оружия по ped[/b] — полный INI в [code]OrcOutFit\Weapons\<dff>.ini[/code] (имя DFF из хука [code]LoadPedObject[/code]), приоритет над [code]OrcOutFit.ini[/code].

[b]Все ped в радиусе[/b] — опция + радиус; кэш на ped; пресеты по DFF имени.

[b]Объекты[/b] — [code]*.dff[/code] в [code]OrcOutFit\Objects[/code]; в [code]<имя>.ini[/code] секции [code][Skin.<dff>][/code] (без папок [code]object\other[/code]).

[b]Скины[/b] — [code]*.dff[/code] в [code]OrcOutFit\Skins[/code]; рендер поверх ped; перед рендером — освещение как у педов ([code]GenerateLightsAffectingObject[/code] + [code]SetLightColoursForPedsCarsAndObjects[/code]), чтобы не было «чёрного» clump; опция скрытия базы.
SA:MP + [code]SkinNickMode[/code]: в основном [b]по нику[/b] ([code][NickBinding][/code]). [code]SkinLocalPreferSelected=1[/code] — выбранный в UI скин на себя [b]в т.ч. при выключенном nick binding[/b]; при включённом nick binding совпадение ника по-прежнему важнее.

[b]UI[/b] — вкладки [b]Main / Weapons / Objects / Skins[/b]. Список стандартных ped: [b]сортировка по model id[/b], формат [code]Имя [ID][/code]. Кнопка [b]Wear this skin[/b] — превью модели со стримингом и безопасной сменой в начале кадра.
[b]Save to Weapons[/b] отключён только для [b]одиночки + дефолтный CJ[/b]; в SA:MP и после примерки — доступно.

[hr]

[size=140][b]Установка[/b][/size]

1) [code]OrcOutFit.asi[/code] в папку игры или modloader (пути от каталога ASI).
2) [code]OrcOutFit.ini[/code] создаётся рядом.
3) Папки: [code]OrcOutFit\Objects[/code], [code]OrcOutFit\Weapons[/code], [code]OrcOutFit\Skins[/code].

[hr]

[size=140][b]Меню[/b][/size]

SP: [code][Main] ActivationKey[/code] (по умолчанию F7).
SA:MP: [code][Main] Command[/code] (по умолчанию [code]/orcoutfit[/code]); опционально [code]SampAllowActivationKey=1[/code].

Известные клиенты: ник по педу; для локального педа при [code]IdFind == 0xFFFF[/code] — id из пула локального игрока (если оффсет в таблице версий не ноль); пробелы по краям ника обрезаются.

[hr]

[size=140][b]Сборка[/b][/size]

[code]"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" OrcOutFit.sln /p:Configuration=Release /p:Platform=x86[/code]

Артефакт: [code]build\Release\OrcOutFit.asi[/code]

[hr]

[size=140][b]OrcOutFit.ini[/b][/size]

[b][Main][/b] — [code]Enabled[/code], [code]ActivationKey[/code], [code]Command[/code], [code]SampAllowActivationKey[/code].

[b][Features][/b] — [code]RenderAllPedsWeapons[/code], [code]RenderAllPedsRadius[/code], [code]ConsiderWeaponSkills[/code], [code]CustomObjects[/code], [code]SkinMode[/code], [code]SkinHideBasePed[/code], [code]SkinNickMode[/code], [code]SkinLocalPreferSelected[/code].

Секции оружия в корне — см. полный [url=https://github.com/dmitriyewich/OrcOutFit/blob/main/README.md]README.md[/url] в репозитории.

[b][SkinMode][/b] — [code]Selected[/code], [code]RandomFromPools[/code] (совместимость; random-пулы в текущей сборке не используются).

[hr]

[size=140][b]Объект .ini[/b][/size]

Секции [code][Skin.<dff>][/code]: кость, offset, rot, scale, [code]ScaleX/Y/Z[/code], опционально [code]Weapons[/code] / [code]WeaponsMode[/code] / [code]HideWeapons[/code].

[hr]

[size=140][b]Скин .ini[/b][/size]

[b][NickBinding][/b] — [code]Enabled[/code], [code]Nicks[/code].

[hr]

[b]Автор:[/b] [url=https://github.com/dmitriyewich]dmitriyewich[/url]
