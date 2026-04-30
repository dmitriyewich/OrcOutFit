#include "orc_ini.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

static std::string IniLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

static std::string IniTrim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

static bool IniReadWholeFile(const char* path, std::string& out) {
    out.clear();
    if (!path || !path[0])
        return false;

    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    const long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    out.resize((size_t)len);
    if (len > 0) {
        const size_t got = fread(&out[0], 1, (size_t)len, f);
        if (got != (size_t)len) {
            fclose(f);
            out.clear();
            return false;
        }
    }
    fclose(f);
    return true;
}

static std::vector<std::string> IniSplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return lines;
}

static void IniEnsureDirectoryForFile(const char* filePath) {
    if (!filePath || !filePath[0])
        return;

    const char* slash = strrchr(filePath, '\\');
    if (!slash)
        slash = strrchr(filePath, '/');
    if (!slash || slash == filePath)
        return;

    std::string path(filePath, slash - filePath);
    for (char& c : path) {
        if (c == '/')
            c = '\\';
    }

    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '\\')
            continue;
        std::string partial = path.substr(0, i);
        if (partial.size() == 2 && partial[1] == ':')
            continue;
        CreateDirectoryA(partial.c_str(), nullptr);
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

static bool IniParseSection(const std::string& line, std::string& section) {
    const std::string t = IniTrim(line);
    if (t.size() < 3 || t[0] != '[')
        return false;
    const size_t close = t.find(']');
    if (close == std::string::npos || close <= 1)
        return false;
    section = IniTrim(t.substr(1, close - 1));
    return !section.empty();
}

static bool IniParseKey(const std::string& line, std::string& key) {
    const std::string t = IniTrim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#')
        return false;
    const size_t eq = t.find('=');
    if (eq == std::string::npos)
        return false;
    key = IniTrim(t.substr(0, eq));
    return !key.empty();
}

bool OrcWriteTextFileAtomic(const char* path, const std::string& text) {
    if (!path || !path[0])
        return false;

    IniEnsureDirectoryForFile(path);

    char tmp[MAX_PATH] = {};
    _snprintf_s(tmp, _TRUNCATE, "%s.tmp.%lu.%lu",
                path,
                (unsigned long)GetCurrentProcessId(),
                (unsigned long)GetTickCount());

    FILE* f = fopen(tmp, "wb");
    if (!f)
        return false;

    bool ok = true;
    if (!text.empty()) {
        ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        DeleteFileA(tmp);
        return false;
    }

    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

bool OrcIniWriteValues(const char* path, const char* initialTextIfMissing, const std::vector<OrcIniValue>& values) {
    if (!path || !path[0])
        return false;
    if (values.empty())
        return true;

    struct PendingValue {
        std::string section;
        std::string key;
        std::string value;
        bool sectionSeen = false;
        bool written = false;
    };

    std::vector<PendingValue> pending;
    std::unordered_map<std::string, size_t> bySectionKey;
    std::unordered_map<std::string, std::vector<size_t>> bySection;

    for (const OrcIniValue& v : values) {
        if (v.section.empty() || v.key.empty())
            continue;
        const std::string secLower = IniLowerAscii(v.section);
        const std::string keyLower = IniLowerAscii(v.key);
        const std::string compound = secLower + "\x1f" + keyLower;
        auto it = bySectionKey.find(compound);
        if (it != bySectionKey.end()) {
            pending[it->second].value = v.value;
            continue;
        }
        PendingValue p;
        p.section = v.section;
        p.key = v.key;
        p.value = v.value;
        const size_t idx = pending.size();
        pending.push_back(std::move(p));
        bySectionKey[compound] = idx;
        bySection[secLower].push_back(idx);
    }

    std::string text;
    if (!IniReadWholeFile(path, text) && initialTextIfMissing)
        text = initialTextIfMissing;

    const std::vector<std::string> lines = IniSplitLines(text);
    std::string out;
    std::string currentSectionLower;

    auto appendLine = [&](const std::string& line) {
        out += line;
        out += '\n';
    };

    auto appendMissingForSection = [&](const std::string& secLower) {
        auto it = bySection.find(secLower);
        if (it == bySection.end())
            return;
        for (size_t idx : it->second) {
            PendingValue& p = pending[idx];
            if (p.written)
                continue;
            out += p.key;
            out += '=';
            out += p.value;
            out += '\n';
            p.written = true;
        }
    };

    for (const std::string& line : lines) {
        std::string section;
        if (IniParseSection(line, section)) {
            if (!currentSectionLower.empty())
                appendMissingForSection(currentSectionLower);
            currentSectionLower = IniLowerAscii(section);
            auto sit = bySection.find(currentSectionLower);
            if (sit != bySection.end()) {
                for (size_t idx : sit->second)
                    pending[idx].sectionSeen = true;
            }
            appendLine(line);
            continue;
        }

        std::string key;
        if (!currentSectionLower.empty() && IniParseKey(line, key)) {
            const std::string compound = currentSectionLower + "\x1f" + IniLowerAscii(key);
            auto vit = bySectionKey.find(compound);
            if (vit != bySectionKey.end()) {
                PendingValue& p = pending[vit->second];
                if (!p.written) {
                    out += p.key;
                    out += '=';
                    out += p.value;
                    out += '\n';
                    p.written = true;
                }
                continue;
            }
        }

        appendLine(line);
    }

    if (!currentSectionLower.empty())
        appendMissingForSection(currentSectionLower);

    for (PendingValue& p : pending) {
        if (p.written || p.sectionSeen)
            continue;
        if (!out.empty() && out.back() != '\n')
            out += '\n';
        if (!out.empty() && out.back() == '\n')
            out += '\n';
        out += '[';
        out += p.section;
        out += "]\n";
        appendMissingForSection(IniLowerAscii(p.section));
    }

    return OrcWriteTextFileAtomic(path, out);
}

bool OrcIniDeleteSection(const char* path, const char* section) {
    if (!path || !path[0] || !section || !section[0])
        return false;

    std::string text;
    if (!IniReadWholeFile(path, text))
        return false;

    const std::vector<std::string> lines = IniSplitLines(text);
    const std::string target = IniLowerAscii(section);
    std::string out;
    bool skipping = false;
    bool found = false;

    for (const std::string& line : lines) {
        std::string sec;
        if (IniParseSection(line, sec)) {
            skipping = (IniLowerAscii(sec) == target);
            if (skipping) {
                found = true;
                continue;
            }
        }
        if (skipping)
            continue;
        out += line;
        out += '\n';
    }

    if (!found)
        return true;
    return OrcWriteTextFileAtomic(path, out);
}
