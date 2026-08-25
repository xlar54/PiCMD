#ifndef BCDC_H
#define BCDC_H
#include "types.h"

// BCDC control frames have a 16-byte dcmd header inside the 496-byte SDPCM
// payload limit.  These routines are synchronous and bounded; a successful
// SET means the firmware replied with status zero, not merely that SDIO sent
// the frame.
bool Bcdc_Ioctl(u32 command, bool set, const u8* input, u32 inputLength,
	u8* output, u32 outputCapacity, u32* outputLength, u32 timeoutUs);
bool Bcdc_GetVar(const char* name, u8* output, u32 outputCapacity);
bool Bcdc_SetVar(const char* name, const u8* value, u32 valueLength);
bool Bcdc_UploadClm(const u8* blob, u32 length);
bool Bcdc_SetCountry(const char country[3]);
bool Bcdc_SetEventMask(const u8* mask, u32 length);
bool Bcdc_EnableAssociationEvents();
bool Bcdc_GetVersion(char* output, u32 capacity);
const char* Bcdc_LastError();
u32 Bcdc_LastStatus();
bool Bcdc_SetInt(const char* name, u32 value);
#endif
