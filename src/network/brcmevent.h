#ifndef BRCMEVENT_H
#define BRCMEVENT_H

#include "types.h"

enum BrcmEventType
{
	BRCM_EVENT_SET_SSID = 0, BRCM_EVENT_START = 2, BRCM_EVENT_AUTH = 3,
	BRCM_EVENT_AUTH_IND = 4, BRCM_EVENT_DEAUTH = 5, BRCM_EVENT_DEAUTH_IND = 6,
	BRCM_EVENT_ASSOC = 7, BRCM_EVENT_ASSOC_IND = 8, BRCM_EVENT_DISASSOC = 11,
	BRCM_EVENT_DISASSOC_IND = 12, BRCM_EVENT_LINK = 16, BRCM_EVENT_PSK_SUP = 46
};
struct BrcmEvent
{
	u32 type, status, reason;
	bool linkUp;
};
struct BrcmEventLog
{
	BrcmEvent events[16];
	u32 totalEventsEver;
	u32 storedEvents;
};

// Validates and persistently captures one SDPCM event-channel payload. It is
// deliberately callable from BCDC reply waits so events cannot disappear just
// because they arrived while a separate control request was in flight.
bool BrcmEvent_Capture(const u8* payload, u32 length);
BrcmEventLog BrcmEvent_GetLog();
void BrcmEvent_ClearLog();

#endif
