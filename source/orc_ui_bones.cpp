#include "orc_ui_bones.h"

#include "orc_types.h"

static const OrcUiBoneRow kBoneRows[] = {
    { 0,              OrcTextId::BoneNone },
    { 1,              OrcTextId::BoneRoot },
    { BONE_PELVIS,    OrcTextId::BonePelvis },
    { BONE_SPINE1,    OrcTextId::BoneSpine1 },
    { 4,              OrcTextId::BoneSpine },
    { 5,              OrcTextId::BoneNeck },
    { 6,              OrcTextId::BoneHead },
    { BONE_R_CLAVIC,  OrcTextId::BoneRightClavicle },
    { BONE_R_UPARM,   OrcTextId::BoneRightUpperArm },
    { 23,             OrcTextId::BoneRightForearm },
    { 24,             OrcTextId::BoneRightHand },
    { BONE_L_CLAVIC,  OrcTextId::BoneLeftClavicle },
    { BONE_L_UPARM,   OrcTextId::BoneLeftUpperArm },
    { 33,             OrcTextId::BoneLeftForearm },
    { 34,             OrcTextId::BoneLeftHand },
    { BONE_L_THIGH,   OrcTextId::BoneLeftThigh },
    { BONE_L_CALF,    OrcTextId::BoneLeftCalf },
    { 43,             OrcTextId::BoneLeftFoot },
    { BONE_R_THIGH,   OrcTextId::BoneRightThigh },
    { BONE_R_CALF,    OrcTextId::BoneRightCalf },
    { 53,             OrcTextId::BoneRightFoot },
};

int OrcUiBoneComboIndex(int boneId) {
    for (int i = 0; i < OrcUiBoneRowCount(); i++)
        if (kBoneRows[i].id == boneId) return i;
    return 0;
}

int OrcUiBoneRowCount() {
    return static_cast<int>(sizeof(kBoneRows) / sizeof(kBoneRows[0]));
}

const OrcUiBoneRow* OrcUiBoneRows() {
    return kBoneRows;
}
