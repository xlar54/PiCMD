// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// See ntpclient.h.
//
// Minimal DHCP, ARP and SNTP implementation over raw Ethernet frames.
// All stages share one deadline and return false on failure.
//
// This file is part of Pi1541.
//
// Pi1541 is free software : you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Pi1541 is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Pi1541. If not, see <http://www.gnu.org/licenses/>.

#include "ntpclient.h"
#include "picmdhd.h"
#include "rpihardware.h"
#include "debug.h"
#include <uspi.h>
#include <string.h>

static const u32 NTP_UNIX_EPOCH_DELTA = 2208988800u;	// 1900-01-01 to 1970-01-01
static NetFrameIO s_frameIO;

// ---------------------------------------------------------------------------
// Byte-order and timing helpers
// ---------------------------------------------------------------------------

static inline void PutBE16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void PutBE32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static inline u16 GetBE16(const u8* p) { return (u16)(((u32)p[0] << 8) | p[1]); }
static inline u32 GetBE32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// BCM microsecond timer; signed subtraction handles wraparound.
static inline bool TimeExpired(u32 deadline)
{
	return (s32)(read32(ARM_SYSTIMER_CLO) - deadline) >= 0;
}

static void SendFramePadded(u8* frame, u32 len)
{
	// Ethernet frames are at least 60 bytes before FCS.
	if (len < 60)
	{
		memset(frame + len, 0, 60 - len);
		len = 60;
	}
	s_frameIO.send(frame, len);
}

// ---------------------------------------------------------------------------
// Internet checksum over big-endian 16-bit words.
// ---------------------------------------------------------------------------

static u32 ChecksumAccumulate(const u8* data, u32 len, u32 sum)
{
	while (len > 1)
	{
		sum += ((u32)data[0] << 8) | data[1];
		data += 2;
		len -= 2;
	}
	if (len)
		sum += (u32)data[0] << 8;
	return sum;
}

static u16 ChecksumFinish(u32 sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (u16)(~sum & 0xFFFF);
}

// ---------------------------------------------------------------------------
// Ethernet + IPv4 + UDP framing
// ---------------------------------------------------------------------------

// Builds an Ethernet/IPv4/UDP frame and returns its length.
static u32 BuildUdpFrame(u8* frame, const u8 dstMac[6], const u8 srcMac[6],
	const u8 srcIP[4], const u8 dstIP[4], u16 srcPort, u16 dstPort,
	const u8* payload, u32 payloadLen)
{
	memcpy(frame, dstMac, 6);
	memcpy(frame + 6, srcMac, 6);
	PutBE16(frame + 12, 0x0800);	// EtherType: IPv4

	u8* ip = frame + 14;
	u32 ipTotalLen = 20 + 8 + payloadLen;
	ip[0] = 0x45;	// version 4, header length 5 * 32bit words
	ip[1] = 0;		// tos
	PutBE16(ip + 2, (u16)ipTotalLen);
	PutBE16(ip + 4, (u16)(read32(ARM_SYSTIMER_CLO) & 0xFFFF));	// id
	PutBE16(ip + 6, 0);	// flags/fragment offset - we never fragment
	ip[8] = 64;			// ttl
	ip[9] = 17;			// protocol: UDP
	PutBE16(ip + 10, 0);	// checksum placeholder
	memcpy(ip + 12, srcIP, 4);
	memcpy(ip + 16, dstIP, 4);
	PutBE16(ip + 10, ChecksumFinish(ChecksumAccumulate(ip, 20, 0)));

	u8* udp = frame + 34;
	u32 udpLen = 8 + payloadLen;
	PutBE16(udp + 0, srcPort);
	PutBE16(udp + 2, dstPort);
	PutBE16(udp + 4, (u16)udpLen);
	PutBE16(udp + 6, 0);	// checksum placeholder
	memcpy(udp + 8, payload, payloadLen);

	u8 pseudo[12];
	memcpy(pseudo, srcIP, 4);
	memcpy(pseudo + 4, dstIP, 4);
	pseudo[8] = 0;
	pseudo[9] = 17;
	PutBE16(pseudo + 10, (u16)udpLen);
	u32 sum = ChecksumAccumulate(pseudo, 12, 0);
	sum = ChecksumAccumulate(udp, udpLen, sum);
	u16 udpChk = ChecksumFinish(sum);
	if (udpChk == 0)
		udpChk = 0xFFFF;	// RFC 768: an all-zero checksum field means "none"
	PutBE16(udp + 6, udpChk);

	return 34 + udpLen;
}

// ---------------------------------------------------------------------------
// ARP
// ---------------------------------------------------------------------------

static u32 BuildArpRequest(u8* frame, const u8 ourMac[6], const u8 ourIP[4], const u8 targetIP[4])
{
	memset(frame, 0xFF, 6);
	memcpy(frame + 6, ourMac, 6);
	PutBE16(frame + 12, 0x0806);	// EtherType: ARP

	u8* a = frame + 14;
	PutBE16(a + 0, 1);			// htype: Ethernet
	PutBE16(a + 2, 0x0800);		// ptype: IPv4
	a[4] = 6;					// hlen
	a[5] = 4;					// plen
	PutBE16(a + 6, 1);			// oper: request
	memcpy(a + 8, ourMac, 6);
	memcpy(a + 14, ourIP, 4);
	memset(a + 18, 0, 6);
	memcpy(a + 24, targetIP, 4);
	return 14 + 28;
}

static bool ResolveArp(const u8 ourMac[6], const u8 ourIP[4], const u8 targetIP[4], u8 resultMac[6], u32 deadline)
{
	u32 nextSend = 0;

	while (!TimeExpired(deadline))
	{
		if (TimeExpired(nextSend))
		{
			u8 req[64];
			SendFramePadded(req, BuildArpRequest(req, ourMac, ourIP, targetIP));
			nextSend = read32(ARM_SYSTIMER_CLO) + 500000;
		}

		u8 rx[64];
		u32 rxLen = 0;
		if (!s_frameIO.receive(rx, sizeof(rx), &rxLen) || rxLen < 42)
			continue;
		if (GetBE16(rx + 12) != 0x0806)
			continue;
		u8* a = rx + 14;
		if (GetBE16(a + 6) != 2)	// oper: reply
			continue;
		if (memcmp(a + 14, targetIP, 4) != 0)	// sender protocol address == who we asked for
			continue;

		memcpy(resultMac, a + 8, 6);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// DHCP (BOOTP framing, RFC 2131)
// ---------------------------------------------------------------------------

struct DhcpOffer
{
	u8 msgType;
	u8 subnetMask[4];
	u8 router[4];
	u8 serverId[4];
	bool hasSubnet;
	bool hasRouter;
	bool hasServerId;
};

static void ParseDhcpOptions(const u8* opts, u32 optsLen, DhcpOffer& out)
{
	u32 i = 0;
	while (i < optsLen)
	{
		u8 type = opts[i++];
		if (type == 0)		// pad
			continue;
		if (type == 0xFF)	// end
			break;
		if (i >= optsLen)
			break;
		u8 len = opts[i++];
		if (i + len > optsLen)
			break;
		const u8* val = opts + i;

		if (type == 53 && len >= 1)
			out.msgType = val[0];
		else if (type == 1 && len >= 4)
		{
			memcpy(out.subnetMask, val, 4);
			out.hasSubnet = true;
		}
		else if (type == 3 && len >= 4)
		{
			memcpy(out.router, val, 4);
			out.hasRouter = true;
		}
		else if (type == 54 && len >= 4)
		{
			memcpy(out.serverId, val, 4);
			out.hasServerId = true;
		}

		i += len;
	}
}

// BOOTP header, magic cookie and options.
static u32 BuildDhcpPayload(u8* p, const u8 ourMac[6], u32 xid, u8 msgType, const u8* requestedIP, const u8* serverId)
{
	memset(p, 0, 236);
	p[0] = 1;	// op: BOOTREQUEST
	p[1] = 1;	// htype: Ethernet
	p[2] = 6;	// hlen
	PutBE32(p + 4, xid);
	PutBE16(p + 10, 0x8000);	// flags: broadcast (we have no IP to receive a unicast reply with yet)
	memcpy(p + 28, ourMac, 6);	// chaddr

	u32 off = 236;
	PutBE32(p + off, 0x63825363);	// magic cookie
	off += 4;

	p[off++] = 53; p[off++] = 1; p[off++] = msgType;

	if (requestedIP)
	{
		p[off++] = 50; p[off++] = 4;
		memcpy(p + off, requestedIP, 4);
		off += 4;
	}
	if (serverId)
	{
		p[off++] = 54; p[off++] = 4;
		memcpy(p + off, serverId, 4);
		off += 4;
	}

	p[off++] = 55; p[off++] = 3; p[off++] = 1; p[off++] = 3; p[off++] = 51;	// param request list: subnet, router, lease time
	p[off++] = 255;	// end

	return off;
}

static const u8 s_broadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const u8 s_zeroIP[4] = { 0, 0, 0, 0 };
static const u8 s_broadcastIP[4] = { 255, 255, 255, 255 };

// Sends a DHCP message and waits for its matching reply.
static bool DhcpPhase(u8 msgType, const u8 ourMac[6], u32 xid, const u8* requestedIP, const u8* serverId,
	u32 deadline, DhcpOffer& outOffer, u8 outYourIP[4])
{
	u8 payload[300];
	u32 plen = BuildDhcpPayload(payload, ourMac, xid, msgType, requestedIP, serverId);

	u32 nextSend = 0;

	while (!TimeExpired(deadline))
	{
		if (TimeExpired(nextSend))
		{
			u8 frame[600];
			u32 flen = BuildUdpFrame(frame, s_broadcastMac, ourMac, s_zeroIP, s_broadcastIP, 68, 67, payload, plen);
			SendFramePadded(frame, flen);
			nextSend = read32(ARM_SYSTIMER_CLO) + 1000000;
		}

		u8 rx[600];
		u32 rxLen = 0;
		if (!s_frameIO.receive(rx, sizeof(rx), &rxLen) || rxLen < 34 + 8 + 240)
			continue;
		if (GetBE16(rx + 12) != 0x0800 || rx[14 + 9] != 17)
			continue;

		u8* udp = rx + 34;
		if (GetBE16(udp + 0) != 67 || GetBE16(udp + 2) != 68)
			continue;

		u8* bootp = udp + 8;
		if (bootp[0] != 2 || GetBE32(bootp + 4) != xid || memcmp(bootp + 28, ourMac, 6) != 0)
			continue;
		if (GetBE32(bootp + 236) != 0x63825363)
			continue;

		memcpy(outYourIP, bootp + 16, 4);	// yiaddr

		outOffer = DhcpOffer();
		u32 udpLen = GetBE16(udp + 4);
		u32 udpPayloadLen = (udpLen > 8) ? (udpLen - 8) : 0;
		u32 optsLen = (udpPayloadLen > 240) ? (udpPayloadLen - 240) : 0;
		u32 rxOptsLen = (rxLen > 34 + 8 + 240) ? (rxLen - (34 + 8 + 240)) : 0;
		if (rxOptsLen < optsLen)
			optsLen = rxOptsLen;
		ParseDhcpOptions(bootp + 240, optsLen, outOffer);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// SNTP (RFC 4330)
// ---------------------------------------------------------------------------

static u32 BuildNtpPayload(u8* p)
{
	memset(p, 0, 48);
	p[0] = 0x23;	// LI = 0, VN = 4, Mode = 3 (client)
	return 48;
}

static bool DoNtp(const u8 ourMac[6], const u8 ourIP[4], const u8 destMac[6], const u8 serverIP[4],
	u32 deadline, u32& outUnixSeconds)
{
	u8 payload[48];
	u32 plen = BuildNtpPayload(payload);
	const u16 srcPort = 12345;

	u32 nextSend = 0;

	while (!TimeExpired(deadline))
	{
		if (TimeExpired(nextSend))
		{
			u8 frame[128];
			u32 flen = BuildUdpFrame(frame, destMac, ourMac, ourIP, serverIP, srcPort, 123, payload, plen);
			SendFramePadded(frame, flen);
			nextSend = read32(ARM_SYSTIMER_CLO) + 1000000;
		}

		u8 rx[128];
		u32 rxLen = 0;
		if (!s_frameIO.receive(rx, sizeof(rx), &rxLen) || rxLen < 34 + 8 + 48)
			continue;
		if (GetBE16(rx + 12) != 0x0800 || rx[14 + 9] != 17)
			continue;
		if (memcmp(rx + 14 + 12, serverIP, 4) != 0)	// IP source address
			continue;

		u8* udp = rx + 34;
		if (GetBE16(udp + 0) != 123 || GetBE16(udp + 2) != srcPort)
			continue;

		u8* ntp = udp + 8;
		if ((ntp[0] & 0x07) != 4)	// mode: server
			continue;

		u32 txSec = GetBE32(ntp + 40);
		if (txSec < NTP_UNIX_EPOCH_DELTA)
			continue;

		outUnixSeconds = txSec - NTP_UNIX_EPOCH_DELTA;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

static bool ParseIPv4(const char* text, u8 out[4])
{
	int part = 0;
	int value = -1;

	for (const char* p = text; ; ++p)
	{
		char c = *p;
		if (c >= '0' && c <= '9')
		{
			if (value < 0)
				value = 0;
			value = value * 10 + (c - '0');
			if (value > 255)
				return false;
		}
		else if (c == '.' || c == '\0')
		{
			if (value < 0 || part >= 4)
				return false;
			out[part++] = (u8)value;
			value = -1;
			if (c == '\0')
				break;
		}
		else
		{
			return false;
		}
	}
	return part == 4;
}

// Converts days since 1970-01-01 to a calendar date.
static void CivilFromDays(s64 z, int& y, int& m, int& d)
{
	z += 719468;
	s64 era = (z >= 0 ? z : z - 146096) / 146097;
	u32 doe = (u32)(z - era * 146097);				// [0, 146096]
	u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;	// [0, 399]
	s64 y0 = (s64)yoe + era * 400;
	u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);	// [0, 365]
	u32 mp = (5 * doy + 2) / 153;						// [0, 11]
	d = (int)(doy - (153 * mp + 2) / 5 + 1);			// [1, 31]
	m = (int)(mp + (mp < 10 ? 3 : -9));				// [1, 12]
	y = (int)(y0 + (m <= 2 ? 1 : 0));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool NtpClient_FetchAndSetTimeOverFrames(PiCMDHD& piCMDHD, const NetFrameIO& io, const u8 ourMac[6], const char* serverIPText, s32 utcOffsetMinutes, u32 timeoutMs)
{
	if (!io.send || !io.receive || !ourMac) return false;
	s_frameIO = io;

	u8 serverIP[4];
	if (!ParseIPv4(serverIPText, serverIP))
	{
		DEBUG_LOG("NTP: invalid NTPServer address '%s'\r\n", serverIPText);
		return false;
	}

	u32 deadline = read32(ARM_SYSTIMER_CLO) + timeoutMs * 1000;
	u32 xid = read32(ARM_SYSTIMER_CLO) ^ 0xC5A9971Bu;

	u8 subnetMask[4] = { 255, 255, 255, 0 };
	u8 gateway[4] = { 0, 0, 0, 0 };

	DhcpOffer offer = {};
	u8 offeredIP[4];
	if (!DhcpPhase(1 /* DISCOVER */, ourMac, xid, 0, 0, deadline, offer, offeredIP) || offer.msgType != 2 || !offer.hasServerId)
	{
		DEBUG_LOG("NTP: no DHCP offer\r\n");
		return false;
	}

	if (offer.hasSubnet)
		memcpy(subnetMask, offer.subnetMask, 4);
	if (offer.hasRouter)
		memcpy(gateway, offer.router, 4);

	DhcpOffer ack = {};
	u8 ackIP[4];
	if (!DhcpPhase(3 /* REQUEST */, ourMac, xid, offeredIP, offer.serverId, deadline, ack, ackIP) || ack.msgType != 5 /* ACK */)
	{
		DEBUG_LOG("NTP: DHCP request not acknowledged\r\n");
		return false;
	}

	const u8* ourIP = offeredIP;

	bool sameSubnet = true;
	for (int i = 0; i < 4; ++i)
	{
		if ((serverIP[i] & subnetMask[i]) != (ourIP[i] & subnetMask[i]))
			sameSubnet = false;
	}
	bool haveGateway = gateway[0] || gateway[1] || gateway[2] || gateway[3];
	if (!sameSubnet && !haveGateway)
	{
		DEBUG_LOG("NTP: server is off-subnet and no gateway was offered\r\n");
		return false;
	}
	const u8* arpTarget = sameSubnet ? serverIP : gateway;

	u8 destMac[6];
	if (!ResolveArp(ourMac, ourIP, arpTarget, destMac, deadline))
	{
		DEBUG_LOG("NTP: ARP resolve failed\r\n");
		return false;
	}

	u32 unixSeconds;
	if (!DoNtp(ourMac, ourIP, destMac, serverIP, deadline, unixSeconds))
	{
		DEBUG_LOG("NTP: no reply from server\r\n");
		return false;
	}

	s64 localSeconds = (s64)unixSeconds + (s64)utcOffsetMinutes * 60;
	if (localSeconds < 0)
		localSeconds = 0;

	s64 days = localSeconds / 86400;
	u32 secOfDay = (u32)(localSeconds % 86400);

	int year, month, day;
	CivilFromDays(days, year, month, day);
	u8 weekday = (u8)((days % 7 + 4) % 7);	// 1970-01-01 (day 0) was a Thursday

	if (year < 2000) year = 2000;
	if (year > 2099) year = 2099;

	piCMDHD.rtc.SetDateTime((u8)(year - 2000), (u8)month, (u8)day, weekday,
		(u8)(secOfDay / 3600), (u8)((secOfDay / 60) % 60), (u8)(secOfDay % 60));

	DEBUG_LOG("NTP: set clock to %04d-%02d-%02d\r\n", year, month, day);
	return true;
}

static bool WiredSend(const u8* frame, u32 length) { USPiSendFrame((u8*)frame, length); return true; }
static bool WiredReceive(u8* frame, u32 capacity, u32* length)
{
	if (!length || !USPiReceiveFrame(frame, length)) return false;
	return *length <= capacity;
}
bool NtpClient_FetchAndSetTime(PiCMDHD& piCMDHD, const char* serverIPText, s32 utcOffsetMinutes, u32 timeoutMs)
{
	if (!USPiEthernetAvailable()) { DEBUG_LOG("NTP: no Ethernet link\r\n"); return false; }
	u8 mac[6]; USPiGetMACAddress(mac); NetFrameIO io = { WiredSend, WiredReceive };
	return NtpClient_FetchAndSetTimeOverFrames(piCMDHD, io, mac, serverIPText, utcOffsetMinutes, timeoutMs);
}
