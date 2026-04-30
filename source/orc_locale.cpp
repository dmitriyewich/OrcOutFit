#include "orc_locale.h"

#include <array>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct OrcTextEntry {
    const char* ru = "";
    const char* en = "";
};

constexpr std::array<OrcTextEntry, static_cast<size_t>(OrcTextId::Count)> kTextEntries = {{
#define ORC_UI_TEXT_ENTRY(id, ru, en) { ru, en },
    ORC_UI_TEXTS(ORC_UI_TEXT_ENTRY)
#undef ORC_UI_TEXT_ENTRY
}};

std::string LowerAscii(const char* value) {
    std::string out = value ? value : "";
    for (char& ch : out) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        ch = static_cast<char>(std::tolower(uch));
    }
    return out;
}

} // namespace

OrcUiLanguage g_orcUiLanguage = OrcUiLanguage::Russian;

OrcUiLanguage OrcParseLanguage(const char* value) {
    const std::string normalized = LowerAscii(value);
    if (normalized == "en" || normalized == "eng" || normalized == "english")
        return OrcUiLanguage::English;
    return OrcUiLanguage::Russian;
}

const char* OrcLanguageId(OrcUiLanguage language) {
    return language == OrcUiLanguage::English ? "en" : "ru";
}

const char* OrcLanguageDisplayName(OrcUiLanguage language) {
    return language == OrcUiLanguage::English
        ? OrcText(OrcTextId::LanguageEnglish)
        : OrcText(OrcTextId::LanguageRussian);
}

const char* OrcText(OrcTextId id) {
    const size_t index = static_cast<size_t>(id);
    if (index >= kTextEntries.size())
        return "";
    const OrcTextEntry& entry = kTextEntries[index];
    return g_orcUiLanguage == OrcUiLanguage::English ? entry.en : entry.ru;
}

std::string OrcFormat(OrcTextId id, ...) {
    const char* format = OrcText(id);

    va_list args;
    va_start(args, id);

    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    if (size <= 0) {
        va_end(args);
        return std::string(format);
    }

    std::vector<char> buffer(static_cast<size_t>(size) + 1u, '\0');
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    return std::string(buffer.data(), static_cast<size_t>(size));
}
