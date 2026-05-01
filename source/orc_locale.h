#pragma once

#include <string>

#define ORC_UI_TEXTS(X) \
    X(WindowTitle, "OrcOutFit", "OrcOutFit") \
    X(Language, "Язык", "Language") \
    X(LanguageRussian, "Русский", "Russian") \
    X(LanguageEnglish, "English", "English") \
    X(Interface, "Интерфейс", "Interface") \
    X(UiAutoScale, "Автомасштаб интерфейса", "Auto-scale UI") \
    X(UiScale, "Масштаб интерфейса", "UI scale") \
    X(UiFontSize, "Размер шрифта", "Font size") \
    X(Activation, "Активация меню", "Menu activation") \
    X(SaveSettings, "Сохранить настройки", "Save settings") \
    X(TabMain, "Главная", "Main") \
    X(TabWeapons, "Оружие", "Weapons") \
    X(TabObjects, "Объекты", "Objects") \
    X(TabSkins, "Скины", "Skins") \
    X(TabSettings, "Настройки", "Settings") \
    X(TabCustomSkins, "Кастомные скины", "Custom skins") \
    X(TabTexture, "Текстуры", "Texture") \
    X(BoneNone, "(нет)", "(none)") \
    X(BoneRoot, "Корень", "Root") \
    X(BonePelvis, "Таз", "Pelvis") \
    X(BoneSpine1, "Позвоночник 1", "Spine1") \
    X(BoneSpine, "Позвоночник", "Spine") \
    X(BoneNeck, "Шея", "Neck") \
    X(BoneHead, "Голова", "Head") \
    X(BoneRightClavicle, "Правая ключица", "R Clavicle") \
    X(BoneRightUpperArm, "Правое плечо", "R UpperArm") \
    X(BoneRightForearm, "Правое предплечье", "R Forearm") \
    X(BoneRightHand, "Правая кисть", "R Hand") \
    X(BoneLeftClavicle, "Левая ключица", "L Clavicle") \
    X(BoneLeftUpperArm, "Левое плечо", "L UpperArm") \
    X(BoneLeftForearm, "Левое предплечье", "L Forearm") \
    X(BoneLeftHand, "Левая кисть", "L Hand") \
    X(BoneLeftThigh, "Левое бедро", "L Thigh") \
    X(BoneLeftCalf, "Левая голень", "L Calf") \
    X(BoneLeftFoot, "Левая ступня", "L Foot") \
    X(BoneRightThigh, "Правое бедро", "R Thigh") \
    X(BoneRightCalf, "Правая голень", "R Calf") \
    X(BoneRightFoot, "Правая ступня", "R Foot") \
    X(WeaponCondition, "Условие по оружию", "Weapon condition") \
    X(WeaponConditionHint, "Выберите оружие, при котором объект будет показываться. Если ничего не выбрано, объект показывается всегда.", "Select weapon(s) that enable this object. If none selected, object renders always.") \
    X(AnySelectedWeapon, "Любое выбранное оружие", "Any selected weapon") \
    X(AllSelectedWeapons, "Все выбранные виды оружия", "All selected weapons") \
    X(HideSelectedWeapons, "Скрывать выбранное оружие на теле, когда показывается объект", "Hide selected weapon(s) on body when object renders") \
    X(ClearWeaponSelection, "Очистить выбор оружия", "Clear weapon selection") \
    X(PluginEnabled, "Плагин включен", "Plugin enabled") \
    X(ToggleKey, "Клавиша меню (SP / опционально SA:MP)", "Toggle key (SP / optional in SA:MP)") \
    X(ToggleKeyHelp, "Допустимые значения: F1..F12, A..Z, 0..9 или числовой VK-код 1..255. В SA:MP клавиша работает только если включена опция ниже; иначе используйте чат-команду.", "Accepted values: F1..F12, A..Z, 0..9, or numeric VK code 1..255. In SA:MP the key works only when the option below is enabled; otherwise use the chat command.") \
    X(ChatCommand, "Команда чата (SA:MP)", "Chat command (SA:MP)") \
    X(SampAllowToggleKey, "SA:MP: также разрешить клавишу меню", "SA:MP: also allow toggle key") \
    X(Features, "Функции", "Features") \
    X(RenderWeaponsForAllPeds, "Рисовать оружие у всех ped", "Render weapons for all peds") \
    X(RenderObjectsForAllPeds, "Рисовать объекты у всех ped", "Render objects for all peds") \
    X(AllPedsRadius, "Радиус всех ped (м)", "All peds radius (m)") \
    X(ConsiderWeaponSkills, "Учитывать навыки оружия (dual wield)", "Consider weapon skills (dual wield)") \
    X(RenderCustomObjects, "Показывать кастомные объекты (папка Objects)", "Render custom objects (Objects folder)") \
    X(SkinMode, "Режим кастомных скинов", "Skin mode (custom Skins)") \
    X(SkinHideBasePed, "Скин: скрывать базового ped", "Skin: hide base ped") \
    X(UnsupportedSampNickBinding, "Неподдерживаемая сборка SA:MP - привязка по нику неактивна (режим SP).", "Unsupported SA:MP build - nick binding inactive (SP mode).") \
    X(SkinNickBinding, "Скин: привязка по нику (SA:MP)", "Skin: nick binding (SA:MP)") \
    X(SkinAlwaysSelectedForMe, "Скин: всегда использовать выбранный скин для меня", "Skin: always use selected skin for me") \
    X(SkinAlwaysSelectedHint, "Если включено: ваш ped использует скин, выбранный во вкладке Скины (после сохранения выбора). Скин по нику все равно имеет приоритет, когда привязка по нику включена и имя совпадает. Работает даже при выключенной привязке по нику.", "If on: your ped uses the skin chosen in the Skins tab (after Save skin mode selection). Nick-bound skin still wins when nick binding is on and your name matches. Works even when nick binding is off.") \
    X(DebugLog, "Отладочный лог (OrcOutFit.log)", "Debug log (OrcOutFit.log)") \
    X(LogOff, "Выкл", "Off") \
    X(LogErrorsOnly, "Только ошибки", "Errors only") \
    X(LogInfoFull, "Информация (полный)", "Info (full)") \
    X(SaveMainFeatures, "Сохранить главное / функции", "Save main / features") \
    X(DataPathFormat, "Объекты: %s", "Objects: %s") \
    X(WeaponsPathFormat, "Оружие: %s", "Weapons: %s") \
    X(SkinsPathFormat, "Скины: %s", "Skins: %s") \
    X(ReloadIni, "Перезагрузить INI", "Reload INI") \
    X(RescanObjects, "Пересканировать объекты", "Rescan objects") \
    X(PedSkinEditingTarget, "Скин ped (цель редактирования) - из ped.dat / кеша LoadPedObject", "Ped skin (editing target) - from ped.dat / LoadPedObject cache") \
    X(NoPedModelsInCacheReconnect, "В кеше пока нет моделей ped (загрузите мир / переподключитесь).", "No ped models in cache yet (load game world / reconnect).") \
    X(NoPedModelsInCache, "В кеше пока нет моделей ped.", "No ped models in cache yet.") \
    X(WearThisSkin, "Надеть этот скин (локальный игрок)", "Wear this skin (local player)") \
    X(Weapon, "Оружие", "Weapon") \
    X(WeaponSlotId, "Слот / ID оружия", "Weapon slot / id") \
    X(EditSecondWeapon, "Редактировать второе оружие (dual wield)", "Edit second weapon (dual wield)") \
    X(WeaponEditorUnavailable, "Редактор оружия недоступен.", "Weapon editor is not available.") \
    X(ShowOnBody, "Показывать на теле", "Show on body") \
    X(Copy, "Копировать", "Copy") \
    X(Paste, "Вставить", "Paste") \
    X(Bone, "Кость", "Bone") \
    X(OffsetX, "Смещение X", "Offset X") \
    X(OffsetY, "Смещение Y", "Offset Y") \
    X(OffsetZ, "Смещение Z", "Offset Z") \
    X(RotationX, "Поворот X (град)", "Rotation X (deg)") \
    X(RotationY, "Поворот Y (град)", "Rotation Y (deg)") \
    X(RotationZ, "Поворот Z (град)", "Rotation Z (deg)") \
    X(Scale, "Масштаб", "Scale") \
    X(ScaleXyz, "Масштаб X/Y/Z", "Scale X/Y/Z") \
    X(SaveToGlobal, "Сохранить глобально (OrcOutFit.ini)", "Save to Global (OrcOutFit.ini)") \
    X(PerSkinPresetDisabledForCj, "Пресет скина: отключен только для CJ в одиночной игре. Используйте SA:MP или кнопку Надеть этот скин, чтобы сменить модель.", "Per-skin preset: disabled for single-player CJ only. Use SA:MP or Wear this skin to change model.") \
    X(SaveToSkinWeapons, "Сохранить для скина (OrcOutFit\\Weapons)", "Save to skin (OrcOutFit\\Weapons)") \
    X(ToggleCommandAndKeyFormat, "Меню: %s | %s", "Toggle: %s | %s") \
    X(ToggleChatFormat, "Меню (чат): %s", "Toggle (chat): %s") \
    X(ToggleKeyFormat, "Клавиша меню: %s", "Toggle key: %s") \
    X(UnsupportedSampSpMode, "Сборка SA:MP не поддерживается - режим SP.", "SA:MP build unsupported - SP mode.") \
    X(NoDffObjectsFolder, "В папке Objects нет *.dff.", "No *.dff in Objects folder.") \
    X(Rescan, "Пересканировать", "Rescan") \
    X(Object, "Объект", "Object") \
    X(PedSkinDffName, "Скин ped (имя DFF)", "Ped skin (DFF name)") \
    X(Show, "Показывать", "Show") \
    X(SaveSkinSectionToObjectIni, "Сохранить [Skin.*] в INI объекта", "Save [Skin.*] to object .ini") \
    X(RescanObjectsFolder, "Пересканировать папку Objects", "Rescan Objects folder") \
    X(CustomSkinsHint, "Кастомные скины - DFF/TXD в папке Skins. Пулы случайного выбора в интерфейсе отключены до реализации.", "Custom skins - DFF/TXD in Skins folder. Random pools UI is disabled until implemented.") \
    X(NoDffSkinsFolder, "В папке Skins нет *.dff.", "No *.dff in Skins folder.") \
    X(Skin, "Скин", "Skin") \
    X(BindSkinToNicks, "Привязать этот скин к никам", "Bind this skin to nick(s)") \
    X(NicksCommaSeparated, "Ники (через запятую).", "Nicks (comma-separated).") \
    X(NickPlaceholder, "Nick1,Nick2", "Nick1,Nick2") \
    X(SaveSkinIni, "Сохранить INI скина", "Save skin .ini") \
    X(SaveSkinModeSelection, "Сохранить выбор режима скина", "Save skin mode selection") \
    X(RescanSkins, "Пересканировать скины", "Rescan skins") \
    X(EnableTextureRemaps, "Включить замену текстур (*_remap)", "Enable texture remaps (*_remap)") \
    X(UnsupportedSampTextureNickBinding, "Неподдерживаемая сборка SA:MP - привязка текстур по нику неактивна.", "Unsupported SA:MP build - texture nick binding inactive.") \
    X(TextureNickBinding, "Привязка текстур по нику (SA:MP)", "Texture nick binding (SA:MP)") \
    X(RandomMode, "Случайный режим", "Random mode") \
    X(RandomModePerTexture, "По каждой текстуре", "Per texture") \
    X(RandomModeLinkedVariant, "Связанный вариант", "Linked variant") \
    X(TextureRemapHint, "Remap в стиле PedFuncs работает на стандартных ped TXD, загруженных игрой.", "PedFuncs-style remap works on standard ped TXDs loaded by the game.") \
    X(SaveTextureSettings, "Сохранить настройки текстур", "Save texture settings") \
    X(NoLocalPedYet, "Локальный ped пока не найден.", "No local ped yet.") \
    X(LocalPedFormat, "Локальный ped: %s [%d]", "Local ped: %s [%d]") \
    X(TxdSlotFormat, "TXD слот: %d", "TXD slot: %d") \
    X(RandomizeLocal, "Случайные текстуры", "Randomize local") \
    X(OriginalTextures, "Вернуть оригинальные текстуры", "Original textures") \
    X(NoRemapTexturesFound, "В загруженном TXD нет текстур *_remap.", "No *_remap textures found in the loaded TXD.") \
    X(Original, "Оригинал", "Original") \
    X(NickBinding, "Привязка по нику", "Nick binding") \
    X(SaveCurrentTextureBinding, "Сохранить привязку", "Save binding") \
    X(ReloadTextureBindings, "Обновить привязки", "Reload bindings") \
    X(TextureNickBindingsCountFormat, "Привязок по никам для этого ped: %d", "Nick bindings for this ped: %d") \
    X(TextureBindingRowFormat, "#%d: %s (%d слот(ов))", "#%d: %s (%d slot(s))") \
    X(Delete, "Удалить", "Delete") \
    X(KnownRemapPedModelsFormat, "Известных ped-моделей с remap: %d", "Known remap ped models: %d") \
    X(TextureStatsFormat, "%s [%d]: %d текстур, %d слот(ов)", "%s [%d]: %d texture(s), %d slot(s)")

enum class OrcUiLanguage {
    Russian = 0,
    English,
};

enum class OrcTextId {
#define ORC_UI_TEXT_ENUM(id, ru, en) id,
    ORC_UI_TEXTS(ORC_UI_TEXT_ENUM)
#undef ORC_UI_TEXT_ENUM
    Count,
};

extern OrcUiLanguage g_orcUiLanguage;

OrcUiLanguage OrcParseLanguage(const char* value);
const char* OrcLanguageId(OrcUiLanguage language);
const char* OrcLanguageDisplayName(OrcUiLanguage language);
const char* OrcText(OrcTextId id);
std::string OrcFormat(OrcTextId id, ...);
