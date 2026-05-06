#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// UTF-8 INI через SimpleIni (SI_NO_CONVERSION в .cpp), семантика GetString/GetInt
// близка к WinAPI GetPrivateProfile* для миграции LoadConfig (этап 2+).

class OrcIniDocument {
public:
    OrcIniDocument();
    ~OrcIniDocument();

    OrcIniDocument(OrcIniDocument&&) noexcept;
    OrcIniDocument& operator=(OrcIniDocument&&) noexcept;

    OrcIniDocument(const OrcIniDocument&) = delete;
    OrcIniDocument& operator=(const OrcIniDocument&) = delete;

    void Clear();
    bool IsLoaded() const;

    bool LoadFromFile(const char* pathUtf8);
    bool LoadFromMemory(const char* data, size_t byteLen);

    std::string GetString(const char* section, const char* key, const char* defaultValue) const;
    int GetInt(const char* section, const char* key, int defaultValue) const;

    bool SectionExists(const char* section) const;
    bool KeyExists(const char* section, const char* key) const;

    /// Имена всех секций (порядок как у SimpleIni `GetAllSections`, не порядок в файле).
    void GetAllSectionNames(std::vector<std::string>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
