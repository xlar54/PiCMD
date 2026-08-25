#include "wifi_data_link.h"
#include "sdpcm.h"
#include "bcdc.h"
#include "brcmevent.h"
static bool Send(const u8* frame,u32 length)
{
	if(!frame||length>492)return false;
	u8 payload[496]={0};
	payload[0]=0x20;
	for(u32 i=0;i<length;++i)payload[4+i]=frame[i];
	return Sdpcm_SendFrame(2,payload,length+4);
}
static bool Receive(u8* frame,u32 capacity,u32* outLength)
{
	if(!frame||!outLength)return false;
	u8 payload[496];
	u8 channel;
	u32 length=0;
	if(!Sdpcm_ReceiveFrame(channel,payload,sizeof(payload),length))return false;
	if(channel==1){BrcmEvent_Capture(payload,length);return false;}
	if(channel!=2||length<4||(payload[0]&0xf0)!=0x20)return false;
	u32 offset=4+((u32)payload[3]<<2);if(offset>length||length-offset>capacity)return false;
	for(u32 i=0;i<length-offset;++i)
		frame[i]=payload[offset+i];
	*outLength=length-offset;
	return true;
}
bool WifiDataLink_GetMac(u8 mac[6]){return mac&&Bcdc_GetVar("cur_etheraddr",mac,6);}
NetFrameIO WifiDataLink_GetFrameIO(){NetFrameIO io={Send,Receive};return io;}
