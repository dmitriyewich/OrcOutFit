#include "orc_weapon_runtime.h"

const char* OrcWeaponRuntimeCompileStamp() {
    return __DATE__ " " __TIME__;
}
void OrcClearAllWeaponReplacementInstances() {
    OrcWeaponClearLocalRendered();
    OrcWeaponClearOtherPedsRendered();
    OrcDestroyAllHeldWeaponReplacementInstances();
}
