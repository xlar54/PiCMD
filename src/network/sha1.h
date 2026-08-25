#ifndef SHA1_H
#define SHA1_H
#include "types.h"

void Wpa2DerivePsk(const u8* passphrase, u32 passphraseLength, const u8* ssid, u32 ssidLength, u8 output[32]);
bool Wpa2PskSelfTest();

#endif
