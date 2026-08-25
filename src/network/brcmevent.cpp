#include "brcmevent.h"

static BrcmEventLog s_log;
static u16 GetBE16(const u8* p) { return ((u16)p[0]<<8)|p[1]; }
static u32 GetBE32(const u8* p) { return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }

bool BrcmEvent_Capture(const u8* payload, u32 length)
{
	if(!payload || length<4 || ((payload[0]>>4)&15)!=2) return false;
	u32 ethernet=4+((u32)payload[3]*4);
	if(ethernet+14+10+48>length) return false;
	if(GetBE16(payload+ethernet+12)!=0x886c) return false;
	u32 brcm=ethernet+14;
	if(GetBE16(payload+brcm)!=0x8001 || payload[brcm+4]!=0 || payload[brcm+5]!=0 || payload[brcm+6]!=0x10 || payload[brcm+7]!=0x18 || GetBE16(payload+brcm+8)!=1) return false;
	u32 event=brcm+10;
	u32 dataLength=GetBE32(payload+event+20);
	if(dataLength>length-(event+48)) return false;
	BrcmEvent parsed;
	parsed.type=GetBE32(payload+event+4);
	parsed.status=GetBE32(payload+event+8);
	parsed.reason=GetBE32(payload+event+12);
	parsed.linkUp=(GetBE16(payload+event+2)&1)!=0;
	u32 index=s_log.totalEventsEver%16;
	s_log.events[index]=parsed;
	++s_log.totalEventsEver;
	if(s_log.storedEvents<16)++s_log.storedEvents;
	return true;
}
BrcmEventLog BrcmEvent_GetLog() { return s_log; }
void BrcmEvent_ClearLog() { s_log.totalEventsEver=0; s_log.storedEvents=0; }
