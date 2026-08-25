#include "bcdc.h"
#include "sdpcm.h"
#include "brcmevent.h"
#include "rpihardware.h"
static const char* s_error;
static u32 s_status;
static u16 s_requestId;
static const u32 BCDC_HEADER = 16;
static const u32 BCDC_MAX_PAYLOAD = 496;
static void Put32(u8* p, u32 value)
{
	p[0]=(u8)value; p[1]=(u8)(value>>8); p[2]=(u8)(value>>16); p[3]=(u8)(value>>24);
}
static u32 Get32(const u8* p)
{
	return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static bool WaitForReply(u16 requestId, u8* reply, u32 capacity, u32& length, u32 timeoutUs)
{
	u8 channel; u32 deadline=read32(ARM_SYSTIMER_CLO)+timeoutUs;
	while((s32)(read32(ARM_SYSTIMER_CLO)-deadline)<0) {
		if(!Sdpcm_ReceiveFrame(channel,reply,capacity,length)){delay_us(1000);continue;}
		if(channel==1) { BrcmEvent_Capture(reply,length); continue; }
		if(channel!=0 || length<16) continue; // Other data/control frames can precede a reply.
		u16 replyId=(u16)((u32)reply[10]|((u32)reply[11]<<8));
		if(replyId!=requestId) continue;
		u32 flags=Get32(reply+8);
		s_status=Get32(reply+12);
		// Status is informational on a successful reply too. Firmware marks a
		// rejected dcmd explicitly with BCDC_DCMD_ERROR (bit 0 of flags).
		if(flags&1u){s_error="BCDC request rejected by firmware";return false;}
		return true;
	}
	s_error="no matching BCDC reply";return false;
}
bool Bcdc_Ioctl(u32 command, bool set, const u8* input, u32 inputLength,
	u8* output, u32 outputCapacity, u32* outputLength, u32 timeoutUs)
{
	if(inputLength>BCDC_MAX_PAYLOAD-BCDC_HEADER || (inputLength&&!input) || (outputCapacity&&!output)) {s_error="invalid BCDC buffer";return false;}
	u8 request[BCDC_MAX_PAYLOAD]={0}, reply[BCDC_MAX_PAYLOAD];
	u16 requestId=++s_requestId;
	Put32(request,command); Put32(request+4,inputLength);
	// This firmware's BCDC implementation uses byte 8 bit 1 for SET and
	// bytes 10..11 for the request ID (the wire layout verified on hardware).
	request[8]=set?2:0; request[10]=(u8)requestId; request[11]=(u8)(requestId>>8);
	for(u32 i=0;i<inputLength;++i) request[BCDC_HEADER+i]=input[i];
	s_status=0;
	if(!Sdpcm_SendFrame(0,request,BCDC_HEADER+inputLength)){s_error="SDPCM control send failed";return false;}
	u32 length=0;
	if(!WaitForReply(requestId,reply,sizeof(reply),length,timeoutUs))return false;
	u32 available=length-BCDC_HEADER, copied=available<outputCapacity?available:outputCapacity;
	for(u32 i=0;i<copied;++i) output[i]=reply[BCDC_HEADER+i];
	if(outputLength)*outputLength=copied;
	s_error="";return true;
}
const char* Bcdc_LastError(){return s_error?s_error:"no BCDC error";}
u32 Bcdc_LastStatus(){return s_status;}
bool Bcdc_GetVar(const char* name, u8* output, u32 outputCapacity)
{
	if(!name || !output || !outputCapacity){s_error="invalid iovar output";return false;}
	u32 nameLength=0;while(name[nameLength])++nameLength;
	if(nameLength+1+outputCapacity>BCDC_MAX_PAYLOAD-BCDC_HEADER){s_error="iovar get too large";return false;}
	u8 request[BCDC_MAX_PAYLOAD-BCDC_HEADER]={0};
	for(u32 i=0;i<nameLength;++i)request[i]=(u8)name[i];
	u32 actual=0;
	return Bcdc_Ioctl(262,false,request,nameLength+1+outputCapacity,output,outputCapacity,&actual,3000000);
}
bool Bcdc_SetVar(const char* name, const u8* value, u32 valueLength)
{
	if(!name || (valueLength&&!value)){s_error="invalid iovar value";return false;}
	u32 nameLength=0;while(name[nameLength])++nameLength;
	if(nameLength+1+valueLength>BCDC_MAX_PAYLOAD-BCDC_HEADER){s_error="iovar set too large";return false;}
	u8 request[BCDC_MAX_PAYLOAD-BCDC_HEADER];
	for(u32 i=0;i<nameLength;++i)
		request[i]=(u8)name[i];
	request[nameLength]=0;
	for(u32 i=0;i<valueLength;++i)request[nameLength+1+i]=value[i];
	return Bcdc_Ioctl(263,true,request,nameLength+1+valueLength,0,0,0,3000000);
}
bool Bcdc_UploadClm(const u8* blob, u32 length)
{
	if(!blob || !length){s_error="invalid CLM blob";return false;}
	// 12 bytes of dload_data + 460 bytes of blob + "clmload\\0" keeps the
	// BCDC payload exactly within the 496-byte SDPCM/CMD53 limit.
	for(u32 offset=0;offset<length;offset+=460) {
		u32 chunk=length-offset; if(chunk>460)chunk=460;
		u8 data[472]={0};
		u16 flags=(u16)(1u<<12); if(!offset)flags|=2; if(offset+chunk==length)flags|=4;
		data[0]=(u8)flags;data[1]=(u8)(flags>>8);data[2]=2;
		Put32(data+4,chunk);
		for(u32 i=0;i<chunk;++i)data[12+i]=blob[offset+i];
		if(!Bcdc_SetVar("clmload",data,12+chunk))return false;
	}
	// Firmware only publishes the final CLM outcome after the DL_END block.
	// A missing status reply is inconclusive, but all chunks being accepted is
	// still stronger evidence than treating a diagnostic read failure as an
	// upload rejection.
	u8 status[4];
	if(!Bcdc_GetVar("clmload_status",status,sizeof(status))){s_error="";s_status=0;return true;}
	if(Get32(status)){s_status=Get32(status);s_error="CLM upload rejected by firmware";return false;}
	return true;
}
bool Bcdc_SetCountry(const char country[3])
{
	if(!country || country[0]<'A'||country[0]>'Z'||country[1]<'A'||country[1]>'Z'||country[2]) {s_error="country must be two uppercase letters";return false;}
	u8 value[12]={0};value[0]=(u8)country[0];value[1]=(u8)country[1];value[8]=(u8)country[0];value[9]=(u8)country[1];
	if(!Bcdc_SetVar("country",value,sizeof(value)))return false;
	u8 readback[12];
	if(!Bcdc_GetVar("country",readback,sizeof(readback)))return false;
	if(readback[0]!=value[0]||readback[1]!=value[1]||readback[8]!=value[8]||readback[9]!=value[9]){s_error="country readback mismatch";return false;}
	return true;
}
bool Bcdc_SetEventMask(const u8* mask, u32 length)
{
	if(!mask||!length){s_error="invalid event mask";return false;}
	return Bcdc_SetVar("event_msgs",mask,length);
}
bool Bcdc_EnableAssociationEvents()
{
	u8 mask[24]={0};
	const u32 events[]={BRCM_EVENT_SET_SSID,BRCM_EVENT_START,BRCM_EVENT_AUTH,BRCM_EVENT_AUTH_IND,
		BRCM_EVENT_DEAUTH,BRCM_EVENT_DEAUTH_IND,BRCM_EVENT_ASSOC,BRCM_EVENT_ASSOC_IND,
		BRCM_EVENT_DISASSOC,BRCM_EVENT_DISASSOC_IND,BRCM_EVENT_LINK,BRCM_EVENT_PSK_SUP};
	for(u32 i=0;i<sizeof(events)/sizeof(events[0]);++i)mask[events[i]/8]|=(u8)(1u<<(events[i]%8));
	return Bcdc_SetEventMask(mask,sizeof(mask));
}
bool Bcdc_GetVersion(char* output,u32 capacity)
{
	if(!output||capacity<2){s_error="invalid output buffer";return false;}
	if(!Bcdc_GetVar("ver",(u8*)output,capacity-1))return false;
	output[capacity-1]=0;return true;
}
bool Bcdc_SetInt(const char* name,u32 value)
{
	u8 bytes[4];Put32(bytes,value);return Bcdc_SetVar(name,bytes,sizeof(bytes));
}
