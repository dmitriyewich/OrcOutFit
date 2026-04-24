[CENTER][SIZE=7][B]OrcOutFit[/B][/SIZE] :good:
[IMG]https://i.imgur.com/kHKmcxq.gif[/IMG]
Коротко: ASI-плагин для GTA San Andreas / SA:MP, который рендерит оружие на теле и добавляет кастомные объекты/скины.
Работает на GTA SA [B]1.0 US[/B], SA:MP: [B]R1, R2, R3, R3-1, R4, R4-2, R5-1, DL-R1
Работает с кастомным оружием, в том числе на Arizona[/B][/CENTER]

[B]Что это?[/B]
OrcOutFit — ASI-плагин для визуальной кастомизации персонажа:
[LIST]
[*]Показывает оружие на скине (текущее оружие в руках не дублируется).
[*]Поддерживает кастомные объекты из папки `OrcOutFit\Objects`.
[*]Поддерживает кастомные скины из `OrcOutFit\Skins`.
[*]Поддерживает замену текстур стандартных скинов через `*_remap` в TXD.
[*]Есть гибкая настройка через in-game меню (Weapons / Objects / Skins / Main).
[/LIST]

[B]Основные возможности[/B]
[LIST]
[*]Пресеты оружия: глобальные и отдельные для конкретных скинов (`OrcOutFit\Weapons\<skin>.ini`).
[*]Рендер оружия и кастомных объектов не только для локального игрока, но и для других скинов (по радиусу).
[*]Рендер объектов кеширует найденные и отсутствующие пары object/skin, чтобы не перечитывать INI каждый кадр.
[*]Dual wield с отдельной настройкой второго оружия, если скилл выдаёт его.
[*]Условный рендер объектов по оружию (Weapons any/all + HideWeapons).
[*]Во вкладке Skins есть две подвкладки: `Custom skins` (DFF поверх скина) и `Texture` (замена `*_remap` текстур).
[*]Texture remap использует реальные найденные текстуры, без PedFuncs-повторов/циклов и белых несуществующих вариантов.
[*]Random mode `Linked variant`: один общий вариант текстур на весь скин, но с fallback на случайный реальный вариант, если у конкретной текстуры вариантов меньше.
[*]Привязка texture remap к нику SA:MP: `OrcOutFit\Skins\Textures\<skin>.ini`. Если binding для ника есть — он важнее random; если нет — используется random.
[*]Live preview в Weapons/Objects: позиция, поворот и масштаб применяются сразу, без обязательного Save.
[*]Debug-лог с уровнями детализации (`OrcOutFit.log`) (по умолчанию отключено).
[/LIST]

[B]Основные проблемы[/B]
[LIST]
[*]Рендер скинов кривой, так как используется система рендера, а не добавление новой модели. Новый скин накладывается на старые кости, что приводит к искажению. Слева - обычная замена, справа - мой скин, посередине - результат рендеринга)[ATTACH width="85px" alt="1776874959292.png"]291825[/ATTACH]
[*]Нет русского языка
[*][B]Скорее не проблема, а фича, но всё же. Все настройки завязаны на название скинов, а не на их ID. То есть расположение оружия будет храниться в файле WMYCLOT.ini[/B]
[*]Texture remap работает только там, где в TXD реально есть текстуры `*_remap`.
[*]Какие-то другие проблемы 100% есть
[/LIST]
Видео c настройкой будет когда я решу что версия стабильная, а пока:
[SPOILER="Скриншоты"][ATTACH width="215px" alt="1776883117166.png"]291848[/ATTACH][ATTACH width="218px" alt="1776875248877.png"]291827[/ATTACH][ATTACH width="171px" alt="1776875283846.png"]291828[/ATTACH][ATTACH alt="1776875313953.png"]291829[/ATTACH][ATTACH width="200px" alt="1776881944046.png"]291842[/ATTACH][ATTACH width="190px" alt="1776883085567.png"]291847[/ATTACH][/SPOILER]

Тестовый (ну или настроенный) конфиг, а также с примерами [B]объектов (скин id 217)[/B] и скинов приложил в [B]OrcOutFit.zip [/B](Папка OrcOutFit и OrcOutFit.ini должны быть рядом с OrcOutFit.asi).

[B]Установка[/B]
[LIST=1]
[*]Поместите `OrcOutFit.asi` рядом с `gta_sa.exe` [B]или[/B] в `scripts` [B]или[/B] в папку `modloader`.
[*]Запустите игру и откройте меню ([B]F7 (или F5) [/B](в самп нужно активировать чекбокс), [B]/orcoutfit[/B] | клавиша/команда настраиваются в INI и в окне).
[*]Рядом с OrcOutFit.asi появится `OrcOutFit.ini`.
[*]При необходимости создайте папки `OrcOutFit\Objects`, `OrcOutFit\Weapons`, `OrcOutFit\Skins`. Texture binding-и сохраняются в `OrcOutFit\Skins\Textures`.
[/LIST]

[B]Исходники[/B]
[URL='https://github.com/dmitriyewich/OrcOutFit']github.com/dmitriyewich/OrcOutFit[/URL]

[B]GitHub Actions[/B]
[LIST]
[*]Workflow: `.github/workflows/build-release-win32.yml`.
[*]Запуск: вручную (`workflow_dispatch`) и при публикации релиза (`release.published`).
[*]CI сначала собирает `Plugin_SA` (`Release|Win32`, `v143`) для `Plugin.lib`, затем `OrcOutFit.sln` (`Release|x86`, `v143`) и загружает `build/Release/OrcOutFit.asi` как artifact/release-asset.
[/LIST]

[RIGHT]:cool:[/RIGHT]
