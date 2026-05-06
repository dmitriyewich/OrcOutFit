#include "orc_ini_document.h"

#define SI_NO_CONVERSION
#include "SimpleIni.h"

#include <cstdlib>
#include <cstring>
#include <vector>

struct OrcIniDocument::Impl {
    CSimpleIniA ini;

    Impl()
        : ini(true, false, false) {
        ini.SetUnicode(true);
        ini.SetMultiKey(false);
        ini.SetMultiLine(false);
    }
};

OrcIniDocument::OrcIniDocument() = default;

OrcIniDocument::~OrcIniDocument() = default;

OrcIniDocument::OrcIniDocument(OrcIniDocument&&) noexcept = default;

OrcIniDocument& OrcIniDocument::operator=(OrcIniDocument&&) noexcept = default;

void OrcIniDocument::Clear() { m_impl.reset(); }

bool OrcIniDocument::IsLoaded() const { return m_impl != nullptr; }

bool OrcIniDocument::LoadFromFile(const char* pathUtf8) {
    Clear();
    if (!pathUtf8 || !pathUtf8[0])
        return false;

    auto impl = std::make_unique<Impl>();
    const SI_Error err = impl->ini.LoadFile(pathUtf8);
    if (err != SI_OK)
        return false;

    m_impl = std::move(impl);
    return true;
}

bool OrcIniDocument::LoadFromMemory(const char* data, size_t byteLen) {
    Clear();
    if (!data && byteLen != 0)
        return false;

    auto impl = std::make_unique<Impl>();
    const SI_Error err = impl->ini.LoadData(data, byteLen);
    if (err != SI_OK)
        return false;

    m_impl = std::move(impl);
    return true;
}

bool OrcIniDocument::SectionExists(const char* section) const {
    if (!m_impl || !section)
        return false;
    return m_impl->ini.SectionExists(section);
}

bool OrcIniDocument::KeyExists(const char* section, const char* key) const {
    if (!m_impl || !section || !key)
        return false;
    return m_impl->ini.KeyExists(section, key);
}

void OrcIniDocument::GetAllSectionNames(std::vector<std::string>& out) const {
    out.clear();
    if (!m_impl)
        return;
    CSimpleIniA::TNamesDepend names;
    m_impl->ini.GetAllSections(names);
    out.reserve(names.size());
    for (const auto& e : names) {
        if (e.pItem && e.pItem[0])
            out.emplace_back(e.pItem);
    }
}

std::string OrcIniDocument::GetString(const char* section, const char* key, const char* defaultValue) const {
    const char* def = defaultValue ? defaultValue : "";
    if (!m_impl || !section || !key)
        return def;
    if (!m_impl->ini.KeyExists(section, key))
        return def;
    const char* v = m_impl->ini.GetValue(section, key, nullptr);
    return v ? std::string(v) : std::string();
}

int OrcIniDocument::GetInt(const char* section, const char* key, int defaultValue) const {
    if (!m_impl || !section || !key)
        return defaultValue;
    if (!m_impl->ini.KeyExists(section, key))
        return defaultValue;

    const char* s = m_impl->ini.GetValue(section, key, nullptr);
    if (!s)
        return defaultValue;

    while (*s == ' ' || *s == '\t')
        ++s;

    if (*s == '\0') {
        // Ключ есть, значение пустое — как GetPrivateProfileInt для пустой строки: 0
        return 0;
    }

    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s)
        return 0;
    return static_cast<int>(v);
}
