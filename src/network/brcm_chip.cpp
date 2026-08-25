// BCM43430 backplane window and the ALP-clock bring-up subset.
#include "brcm_chip.h"
#include "wifi_sdio.h"
#include "rpihardware.h"
static const BrcmChipInfo s_43430 = { 43430, 1, 0x18000000, 0x18002000, 0x18103000, 0x18003000, 0x18101000, 0x18104000, 0x18004000, 0, 524288 };
static u32 s_window = 0xFFFFFFFF; static const char* s_error;
static bool Expired(u32 d) { return (s32)(read32(ARM_SYSTIMER_CLO)-d)>=0; }
static bool Window(u32 address) {
	u32 base=address&~0x7FFFu; if(base==s_window) return true; u8 v;
	if(!WifiSdio_Cmd52(true,1,0x1000A,(u8)(base>>8),&v)||!WifiSdio_Cmd52(true,1,0x1000B,(u8)(base>>16),&v)||!WifiSdio_Cmd52(true,1,0x1000C,(u8)(base>>24),&v)) { s_error=WifiSdio_LastError(); return false; }
	s_window=base; return true;
}
static bool Read32(u32 address,u32& out) { if(!Window(address))return false; u8 b[4]; if(!WifiSdio_Cmd53(false,1,(address&0x7FFF)|0x8000,true,b,4)){s_error=WifiSdio_LastError();return false;} out=(u32)b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24);return true; }
static bool Write32(u32 address, u32 value) { u8 b[4]={(u8)value,(u8)(value>>8),(u8)(value>>16),(u8)(value>>24)}; return BrcmChip_BackplaneWriteBlock(address,b,4); }
bool BrcmChip_Read32(u32 address,u32& value){return Read32(address,value);}
bool BrcmChip_Write32(u32 address,u32 value){return Write32(address,value);}
bool BrcmChip_Identify(BrcmChipInfo& out) { s_window=0xFFFFFFFF; u32 id; if(!Read32(s_43430.chipCommonBase,id))return false; if((id&0xFFFF)!=43430){s_error="unexpected Broadcom chip";return false;} out=s_43430; out.revision=(u16)((id>>16)&0xF); return true; }
bool BrcmChip_BackplaneWriteBlock(u32 address, const u8* data, u32 length) {
	if (!data || !length || (address & 3) || (length & 3)) { s_error="unaligned backplane write"; return false; }
	while (length) { if (!Window(address)) return false; u32 room=0x8000-(address&0x7FFF), chunk=length; if(chunk>64)chunk=64; if(chunk>room)chunk=room;
		if (!WifiSdio_Cmd53(true,1,(address&0x7FFF)|0x8000,true,(u8*)data,chunk)){s_error=WifiSdio_LastError();return false;} address+=chunk;data+=chunk;length-=chunk; }
	return true;
}
static bool CoreDisable(u32 wrapper, u32 pre, u32 reset) {
	u32 v; if(!Read32(wrapper+0x800,v))return false;
	if(!(v&1)) { if(!Write32(wrapper+0x408,pre|3)||!Write32(wrapper+0x800,1))return false; delay_us(20); }
	return Write32(wrapper+0x408,reset|3);
}
static bool CoreReset(u32 wrapper,u32 pre,u32 reset,u32 post) {
	if(!CoreDisable(wrapper,pre,reset))return false; u32 v=1;
	for(unsigned i=0;i<50&&v;i++){if(!Read32(wrapper+0x800,v))return false;if(v){if(!Write32(wrapper+0x800,0))return false;delay_us(50);}}
	if(v){s_error="core reset timeout";return false;} return Write32(wrapper+0x408,post|1);
}
bool BrcmChip_SetPassive(const BrcmChipInfo& c) {
	if(!CoreDisable(c.armCtlBase,0,0)||!CoreReset(c.d11Base,0xC,4,4)||!CoreReset(c.socramCtlBase,0,0,0))return false;
	return Write32(c.socramRegsBase+0x10,3)&&Write32(c.socramRegsBase+0x44,0);
}
bool BrcmChip_StartFirmware(const BrcmChipInfo& c, const u8 resetVector[4]) {
	if (!BrcmChip_BackplaneWriteBlock(c.ramBase, resetVector, 4) || !CoreReset(c.armCtlBase,0,0,0)) return false;
	WifiSdio_Cmd52(true,1,0x1000E,0,0); delay_us(1000);
	if (!WifiSdio_Cmd52(true,1,0x1000E,0x10,0)) { s_error=WifiSdio_LastError(); return false; }
	u32 deadline=read32(ARM_SYSTIMER_CLO)+5000000; u8 v=0;
	do { if(!WifiSdio_Cmd52(false,1,0x1000E,0,&v)){s_error=WifiSdio_LastError();return false;} if((v&0xC0)==0xC0) break; delay_us(10); } while(!Expired(deadline));
	if((v&0xC0)!=0xC0){s_error="HT clock timeout";return false;}
	if(!WifiSdio_Cmd52(true,1,0x1000E,(u8)(v|2),0) || !Write32(c.sdioCoreBase+0x48,4u<<16) || !WifiSdio_EnableFunction(2,1000000)){s_error=WifiSdio_LastError();return false;}
	return true;
}
bool BrcmChip_RequestAlp(BrcmAlpDebug& out,u32 timeoutUs) { u8 v=0; if(!WifiSdio_Cmd52(false,1,0x1000E,0,&v)){s_error=WifiSdio_LastError();return false;} out.before=v; if(!WifiSdio_Cmd52(true,1,0x1000E,0x20|0x08,0)){s_error=WifiSdio_LastError();return false;} out.requested=0x28; u32 d=read32(ARM_SYSTIMER_CLO)+timeoutUs; do {if(!WifiSdio_Cmd52(false,1,0x1000E,0,&v)){s_error=WifiSdio_LastError();return false;}if(v&0x40)break;delay_us(10);}while(!Expired(d)); if(!(v&0x40)){s_error="ALP clock timeout";return false;} if(!WifiSdio_Cmd52(true,1,0x1000E,0x20|0x01,0)){s_error=WifiSdio_LastError();return false;} delay_us(65); WifiSdio_Cmd52(true,1,0x1000F,0,0); out.finalValue=v; s_error=""; return true; }
const char* BrcmChip_LastError(){return s_error?s_error:"not initialised";}
