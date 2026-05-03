#pragma once

#include "orc_locale.h"

struct OrcUiBoneRow {
    int id;
    OrcTextId label;
};

int OrcUiBoneComboIndex(int boneId);
int OrcUiBoneRowCount();
const OrcUiBoneRow* OrcUiBoneRows();
