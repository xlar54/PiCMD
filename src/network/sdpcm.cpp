#include "sdpcm.h"
#include "wifi_sdio.h"
#include "brcm_chip.h"
#include "rpihardware.h"
static u8 s_sequence;
static const char* s_error;
static void Put16(u8* p,u16 v){p[0]=(u8)v;p[1]=(u8)(v>>8);}
static void Put32(u8* p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
bool Sdpcm_WaitReady(const BrcmChipInfo& c,u32 timeoutUs){
	if(!BrcmChip_Write32(c.sdioCoreBase+0x24,0xF0)){s_error="cannot set host interrupt mask";return false;}u32 d=read32(ARM_SYSTIMER_CLO)+timeoutUs;
	do{u32 status,mail;if(!BrcmChip_Read32(c.sdioCoreBase+0x20,status)||!BrcmChip_Read32(c.sdioCoreBase+0x4c,mail))return false;
		if(mail){BrcmChip_Write32(c.sdioCoreBase+0x40,2);if(mail&0xA){BrcmChip_Write32(c.sdioCoreBase+0x20,status&~0x40);return true;}}delay_us(1000);
 	}while((s32)(read32(ARM_SYSTIMER_CLO)-d)<0);s_error="firmware-ready mailbox timeout";return false;}
bool Sdpcm_SendFrame(u8 channel,const u8* payload,u32 payloadLength)
{
	if(channel>15 || payloadLength>496 || (payloadLength && !payload)){s_error="invalid SDPCM frame";return false;}
	u8 frame[508]; u32 length=12+payloadLength, padded=(length+3)&~3u;
	for(u32 i=0;i<padded;++i)frame[i]=0;
	Put16(frame,(u16)length); Put16(frame+2,(u16)~length);
	Put32(frame+4,(u32)s_sequence|((u32)channel<<8)|(12u<<24));
	if(payloadLength) for(u32 i=0;i<payloadLength;++i)frame[12+i]=payload[i];
	if(!WifiSdio_Cmd53(true,2,0,false,frame,padded)){s_error=WifiSdio_LastError();return false;}
	++s_sequence; s_error="";return true;
}
bool Sdpcm_ReceiveFrame(u8& channel,u8* payload,u32 capacity,u32& payloadLength)
{
	// BCM43430's Function-2 FIFO must be drained with an initial 64-byte
	// read; a 12-byte header-only CMD53 leaves its FIFO framing out of sync.
	u8 header[64]; payloadLength=0;
	if(!WifiSdio_Cmd53(false,2,0,false,header,64)){s_error=WifiSdio_LastError();return false;}
	u16 length=(u16)header[0]|((u16)header[1]<<8), inverse=(u16)header[2]|((u16)header[3]<<8);
	if(!length){s_error="SDPCM FIFO returned empty frame";return false;}
	if((u16)~length!=inverse || length<12 || length>508){s_error="invalid SDPCM header";return false;}
	u32 word=(u32)header[4]|((u32)header[5]<<8)|((u32)header[6]<<16)|((u32)header[7]<<24);
	channel=(u8)(word>>8); u32 dataOffset=word>>24;
	if(dataOffset<12||dataOffset>length||length-dataOffset>capacity){s_error="SDPCM payload too large";return false;}
	payloadLength=length-dataOffset;
	// The FIFO advances per CMD53 transaction; normal firmware frames used by
	// BCDC have a 12-byte header, so fetch their payload in one aligned read.
	u32 padded=(length+3)&~3u; u8 scratch[508];
	if (padded <= 64) { for(u32 i=0;i<payloadLength;++i)payload[i]=header[dataOffset+i]; return true; }
	padded -= 64;
	if(padded && (!WifiSdio_Cmd53(false,2,0,false,scratch,padded))){s_error=WifiSdio_LastError();return false;}
	for(u32 i=0;i<payloadLength;++i) { u32 source=dataOffset+i; payload[i]=(source<64)?header[source]:scratch[source-64]; }
	return true;
}
const char* Sdpcm_LastError(){return s_error?s_error:"not initialised";}
