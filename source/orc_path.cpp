#include "orc_path.h"

#include <windows.h>

std::string OrcJoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

bool OrcFileExistsA(const char* path) {
    if (!path) return false;
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t OrcFileLastWriteUtcTicks(const char* path) {
    if (!path || !path[0]) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return 0;
    return (uint64_t)fad.ftLastWriteTime.dwLowDateTime |
           ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32u);
}

std::string OrcBaseNameNoExt(const std::string& file) {
    size_t slash = file.find_last_of("\\/");
    size_t start = (slash == std::string::npos) ? 0 : (slash + 1);
    size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot < start) dot = file.size();
    return file.substr(start, dot - start);
}

std::string OrcLowerExt(const std::string& file) {
    size_t dot = file.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = file.substr(dot);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return ext;
}

std::string OrcToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string OrcFindBestTxdPath(const std::string& dir, const std::string& base) {
    std::string mask = OrcJoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};

    const std::string baseLo = OrcToLowerAscii(base);
    std::string fallbackSingle;
    int txdCount = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (OrcLowerExt(fname) != ".txd") continue;
        txdCount++;
        const std::string fbase = OrcBaseNameNoExt(fname);
        if (OrcToLowerAscii(fbase) == baseLo) {
            FindClose(h);
            return OrcJoinPath(dir, fname);
        }
        if (fallbackSingle.empty()) fallbackSingle = OrcJoinPath(dir, fname);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (txdCount == 1) return fallbackSingle;
    return {};
}
