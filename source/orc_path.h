#pragma once

#include <string>

std::string OrcJoinPath(const std::string& a, const std::string& b);
bool OrcFileExistsA(const char* path);
std::string OrcBaseNameNoExt(const std::string& file);
std::string OrcLowerExt(const std::string& file);
std::string OrcToLowerAscii(std::string s);
std::string OrcFindBestTxdPath(const std::string& dir, const std::string& base);
