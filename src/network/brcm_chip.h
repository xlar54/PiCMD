#ifndef BRCM_CHIP_H
#define BRCM_CHIP_H
#include "types.h"
struct BrcmChipInfo { u16 chipId, revision; u32 chipCommonBase, sdioCoreBase, armCtlBase, armRegsBase, d11Base, socramCtlBase, socramRegsBase, ramBase, ramSize; };
struct BrcmAlpDebug { u8 before, requested, finalValue; };
bool BrcmChip_Identify(BrcmChipInfo& out);
bool BrcmChip_RequestAlp(BrcmAlpDebug& out, u32 timeoutUs);
bool BrcmChip_BackplaneWriteBlock(u32 address, const u8* data, u32 length);
bool BrcmChip_SetPassive(const BrcmChipInfo& chip);
bool BrcmChip_StartFirmware(const BrcmChipInfo& chip, const u8 resetVector[4]);
bool BrcmChip_Read32(u32 address, u32& value);
bool BrcmChip_Write32(u32 address, u32 value);
const char* BrcmChip_LastError();
#endif
