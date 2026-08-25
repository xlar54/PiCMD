#ifndef SDPCM_H
#define SDPCM_H
#include "types.h"
struct BrcmChipInfo;
bool Sdpcm_WaitReady(const BrcmChipInfo& chip, u32 timeoutUs);
// Sends one SDPCM frame over BCM43430 SDIO function 2. Payload is limited to
// 496 bytes here because the raw byte-mode CMD53 transport is capped at 508.
bool Sdpcm_SendFrame(u8 channel, const u8* payload, u32 payloadLength);
bool Sdpcm_ReceiveFrame(u8& channel, u8* payload, u32 capacity, u32& payloadLength);
const char* Sdpcm_LastError();
#endif
