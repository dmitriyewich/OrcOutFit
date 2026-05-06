#pragma once

#include "orc_ini_document.h"

/// Кеш `OrcIniDocument` по пути UTF-8: перезагрузка при смене mtime на диске.
/// После правок INI — `OrcIniCacheInvalidatePath` для конкретного файла или `OrcIniCacheInvalidateAll()`.
const OrcIniDocument* OrcIniCacheGet(const char* pathUtf8);
void OrcIniCacheInvalidatePath(const char* pathUtf8);
void OrcIniCacheInvalidateAll();
