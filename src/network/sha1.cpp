#include "sha1.h"

struct Sha1State { u32 h[5]; u64 bytes; u8 block[64]; u32 used; };
static u32 Rol(u32 v,u32 n){return (v<<n)|(v>>(32-n));}
static void Block(Sha1State& s,const u8* p)
{
	u32 w[80];for(u32 i=0;i<16;++i)w[i]=((u32)p[i*4]<<24)|((u32)p[i*4+1]<<16)|((u32)p[i*4+2]<<8)|p[i*4+3];
	for(u32 i=16;i<80;++i)w[i]=Rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
	u32 a=s.h[0],b=s.h[1],c=s.h[2],d=s.h[3],e=s.h[4];
	for(u32 i=0;i<80;++i){u32 f,k;if(i<20){f=(b&c)|((~b)&d);k=0x5a827999;}else if(i<40){f=b^c^d;k=0x6ed9eba1;}else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;}else{f=b^c^d;k=0xca62c1d6;}u32 t=Rol(a,5)+f+e+k+w[i];e=d;d=c;c=Rol(b,30);b=a;a=t;}
	s.h[0]+=a;s.h[1]+=b;s.h[2]+=c;s.h[3]+=d;s.h[4]+=e;
}
static void Init(Sha1State& s){s.h[0]=0x67452301;s.h[1]=0xefcdab89;s.h[2]=0x98badcfe;s.h[3]=0x10325476;s.h[4]=0xc3d2e1f0;s.bytes=0;s.used=0;}
static void Update(Sha1State& s,const u8* p,u32 n){s.bytes+=n;while(n){u32 take=64-s.used;if(take>n)take=n;for(u32 i=0;i<take;++i)s.block[s.used+i]=p[i];s.used+=take;p+=take;n-=take;if(s.used==64){Block(s,s.block);s.used=0;}}}
static void Final(Sha1State& s,u8 out[20]){u64 bits=s.bytes*8;s.block[s.used++]=0x80;if(s.used>56){while(s.used<64)s.block[s.used++]=0;Block(s,s.block);s.used=0;}while(s.used<56)s.block[s.used++]=0;for(u32 i=0;i<8;++i)s.block[56+i]=(u8)(bits>>(56-i*8));Block(s,s.block);for(u32 i=0;i<5;++i){out[i*4]=(u8)(s.h[i]>>24);out[i*4+1]=(u8)(s.h[i]>>16);out[i*4+2]=(u8)(s.h[i]>>8);out[i*4+3]=(u8)s.h[i];}}
static void Hmac(const u8* key,u32 keyLength,const u8* data,u32 dataLength,u8 out[20]){u8 k[64]={0},inner[20];if(keyLength>64){Sha1State x;Init(x);Update(x,key,keyLength);Final(x,k);}else for(u32 i=0;i<keyLength;++i)k[i]=key[i];u8 ipad[64],opad[64];for(u32 i=0;i<64;++i){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}Sha1State s;Init(s);Update(s,ipad,64);Update(s,data,dataLength);Final(s,inner);Init(s);Update(s,opad,64);Update(s,inner,20);Final(s,out);}
void Wpa2DerivePsk(const u8* pass,u32 passLen,const u8* ssid,u32 ssidLen,u8 output[32])
{
	u8 input[68],u[20],t[20];for(u32 i=0;i<ssidLen;++i)input[i]=ssid[i];
	for(u32 block=1;block<=2;++block){input[ssidLen]=(u8)(block>>24);input[ssidLen+1]=(u8)(block>>16);input[ssidLen+2]=(u8)(block>>8);input[ssidLen+3]=(u8)block;Hmac(pass,passLen,input,ssidLen+4,u);for(u32 j=0;j<20;++j)t[j]=u[j];for(u32 round=1;round<4096;++round){Hmac(pass,passLen,u,20,u);for(u32 j=0;j<20;++j)t[j]^=u[j];}u32 take=block==1?20:12;for(u32 j=0;j<take;++j)output[(block-1)*20+j]=t[j];}
}
bool Wpa2PskSelfTest(){static const u8 pass[]="password",ssid[]="IEEE";static const u8 expected[32]={0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};u8 actual[32];Wpa2DerivePsk(pass,8,ssid,4,actual);for(u32 i=0;i<32;++i)if(actual[i]!=expected[i])return false;return true;}
