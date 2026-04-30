#pragma once

#include <string>
#include <vector>

struct OrcIniValue {
    std::string section;
    std::string key;
    std::string value;
};

bool OrcWriteTextFileAtomic(const char* path, const std::string& text);
bool OrcIniWriteValues(const char* path, const char* initialTextIfMissing, const std::vector<OrcIniValue>& values);
bool OrcIniDeleteSection(const char* path, const char* section);
