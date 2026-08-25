#include "wpa2.h"
#include "sha1.h"
#include "bcdc.h"
#include "brcmevent.h"
#include "sdpcm.h"
#include "rpihardware.h"
static u32 Length(const char* s){u32 n=0;while(s&&s[n])++n;return n;}
static void Put32(u8* p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
static void Apply(const BrcmEvent& e,Wpa2Debug& d,Wpa2Result& result){if(e.type==BRCM_EVENT_SET_SSID){if(e.status)d.failure="SET_SSID event failed",result=WPA2_FAILED;else d.setSsidSeen=true;}else if(e.type==BRCM_EVENT_PSK_SUP){if(e.status==6)d.keyedSeen=true;else if(e.status==7)d.failure="WPA2 handshake timed out",result=WPA2_FAILED;}else if(e.type==BRCM_EVENT_DEAUTH||e.type==BRCM_EVENT_DEAUTH_IND||e.type==BRCM_EVENT_DISASSOC||e.type==BRCM_EVENT_DISASSOC_IND){d.failure="deauthenticated",result=WPA2_FAILED;}if(d.setSsidSeen&&d.keyedSeen)result=WPA2_ASSOCIATED;}
static void ReplayFrom(u32 baseline,Wpa2Debug& d,Wpa2Result& result){BrcmEventLog log=BrcmEvent_GetLog();u32 first=log.totalEventsEver-log.storedEvents;if(baseline>first)first=baseline;for(u32 n=first;n<log.totalEventsEver;++n){Apply(log.events[n%16],d,result);++d.eventCount;}}
bool Wpa2_RunSelfTest(){return Wpa2PskSelfTest();}
Wpa2Result Wpa2_Associate(const char* ssid,const char* pass,u32 timeoutUs,Wpa2Debug& d)
{
	d.pskSelfTest=Wpa2PskSelfTest();d.setSsidSeen=false;d.keyedSeen=false;d.eventCount=0;d.failure="";
	u32 ssidLen=Length(ssid),passLen=Length(pass);if(!d.pskSelfTest){d.failure="PBKDF2 self-test failed";return WPA2_CONFIGURATION_ERROR;}if(!ssidLen||ssidLen>32||passLen<8||passLen>63){d.failure="invalid SSID or passphrase length";return WPA2_INVALID_CREDENTIALS;}
	u8 integer[4];Put32(integer,1);if(!Bcdc_Ioctl(20,true,integer,4,0,0,0,3000000)){d.failure="set infrastructure failed";return WPA2_CONFIGURATION_ERROR;}
	static const u8 rsn[]={0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00};
	if(!Bcdc_SetVar("wpaie",rsn,sizeof(rsn))||!Bcdc_SetInt("wpa_auth",0xc0)||!Bcdc_SetInt("auth",0)||!Bcdc_SetInt("wsec",4)){d.failure="security iovar failed";return WPA2_CONFIGURATION_ERROR;}
	Bcdc_SetInt("mfp",0); // Optional: old BCM43430 firmware may not provide it.
	if(!Bcdc_SetInt("wpa_auth",0x80)||!Bcdc_SetInt("sup_wpa",1)){d.failure="WPA2 supplicant setup failed";return WPA2_CONFIGURATION_ERROR;}
	u8 pmk[32],pmkRequest[132]={0};Wpa2DerivePsk((const u8*)pass,passLen,(const u8*)ssid,ssidLen,pmk);pmkRequest[0]=32;for(u32 i=0;i<32;++i)pmkRequest[4+i]=pmk[i];
	if(!Bcdc_Ioctl(268,true,pmkRequest,sizeof(pmkRequest),0,0,0,3000000)){d.failure="PMK setup failed";return WPA2_CONFIGURATION_ERROR;}delay_us(10000);
	u32 baseline=BrcmEvent_GetLog().totalEventsEver;u8 join[36]={0};Put32(join,ssidLen);for(u32 i=0;i<ssidLen;++i)join[4+i]=(u8)ssid[i];
	if(!Bcdc_Ioctl(26,true,join,sizeof(join),0,0,0,3000000)){d.failure="join request failed";return WPA2_CONFIGURATION_ERROR;}
	Wpa2Result result=WPA2_INCONCLUSIVE;ReplayFrom(baseline,d,result);u32 deadline=read32(ARM_SYSTIMER_CLO)+timeoutUs;
	while(result==WPA2_INCONCLUSIVE&&(s32)(read32(ARM_SYSTIMER_CLO)-deadline)<0){u8 payload[496];u8 channel;u32 length;if(!Sdpcm_ReceiveFrame(channel,payload,sizeof(payload),length))continue;if(channel==1&&BrcmEvent_Capture(payload,length))ReplayFrom(baseline,d,result);}
	if(result==WPA2_INCONCLUSIVE)
		d.failure="association timeout";
	return result;
}
