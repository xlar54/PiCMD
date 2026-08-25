#ifndef WPA2_H
#define WPA2_H
#include "types.h"
enum Wpa2Result { WPA2_ASSOCIATED, WPA2_FAILED, WPA2_INCONCLUSIVE, WPA2_CONFIGURATION_ERROR, WPA2_INVALID_CREDENTIALS };
struct Wpa2Debug { bool pskSelfTest, setSsidSeen, keyedSeen; u32 eventCount; const char* failure; };
bool Wpa2_RunSelfTest();
Wpa2Result Wpa2_Associate(const char* ssid, const char* passphrase, u32 timeoutUs, Wpa2Debug& debug);
#endif
