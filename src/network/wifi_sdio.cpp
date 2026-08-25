// Minimal SDIO host for Pi 3's BCM43430. Register sequencing follows the
// Arasan SDHCI setup already used by rpi/emmc.cpp and SDIO Simplified Spec.
#include "wifi_sdio.h"
#include "rpihardware.h"
#include "rpi-gpio.h"

#define EMMC_BLKSIZECNT (ARM_EMMC_BASE + 0x04)
#define EMMC_ARG1       (ARM_EMMC_BASE + 0x08)
#define EMMC_CMDTM      (ARM_EMMC_BASE + 0x0C)
#define EMMC_RESP0      (ARM_EMMC_BASE + 0x10)
#define EMMC_DATA       (ARM_EMMC_BASE + 0x20)
#define EMMC_STATUS     (ARM_EMMC_BASE + 0x24)
#define EMMC_CONTROL1   (ARM_EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT  (ARM_EMMC_BASE + 0x30)
#define EMMC_IRPT_MASK  (ARM_EMMC_BASE + 0x34)
#define EMMC_IRPT_EN    (ARM_EMMC_BASE + 0x38)

static bool s_onWlan;
static bool s_everInitialized;
static u16 s_rca;
static const char* s_error;

static bool Expired(u32 deadline) { return (s32)(read32(ARM_SYSTIMER_CLO) - deadline) >= 0; }
static void SetError(const char* error) { s_error = error; }
static void SetGroup(rpi_gpio_pin_t first, unsigned count, rpi_gpio_alt_function_t function)
{
	for (unsigned i = 0; i < count; ++i)
	{
		rpi_gpio_pin_t gpio = (rpi_gpio_pin_t)(first + i);
		rpi_reg_rw_t* reg = &RPI_GpioBase->GPFSEL[gpio / 10];
		rpi_reg_rw_t value = *reg;
		value &= ~(FS_MASK << ((gpio % 10) * 3));
		value |= ((unsigned)function << ((gpio % 10) * 3));
		*reg = value;
	}
}
static bool WaitReg(u32 reg, u32 mask, bool set, u32 timeoutUs)
{
	u32 deadline = read32(ARM_SYSTIMER_CLO) + timeoutUs;
	while (!Expired(deadline))
	{
		if (((read32(reg) & mask) != 0) == set) return true;
		// Avoid a millisecond penalty in the normal SDIO register-settle case.
		for (unsigned i = 0; i < 10; ++i) delay_us(5);
	}
	return false;
}
static void ResetDataLine()
{
	write32(EMMC_CONTROL1, read32(EMMC_CONTROL1) | (1u << 26));
	WaitReg(EMMC_CONTROL1, 1u << 26, false, 100000);
}
static bool ResetController()
{
	write32(EMMC_CONTROL1, (read32(EMMC_CONTROL1) & ~5u) | (1u << 24));
	if (!WaitReg(EMMC_CONTROL1, 7u << 24, false, 1000000)) { SetError("EMMC reset timeout"); return false; }
	return true;
}
static u32 Divider(u32 base, u32 rate)
{
	u32 div = (base + rate - 1) / rate;
	if (div < 2) div = 2;
	if (div & 1) ++div;
	return div >> 1;
}
static bool SetClock(u32 rate, bool fromStopped)
{
	u32 c = read32(EMMC_CONTROL1) & ~((0x3FFu << 6) | 5u | (0xFu << 16));
	u32 div = Divider((u32)get_clock_rate(CLOCK_ID_EMMC), rate);
	c |= ((div & 0xFFu) << 8) | ((div & 0x300u) >> 2) | (0xEu << 16);
	write32(EMMC_CONTROL1, c | 1u);
	if (fromStopped && !WaitReg(EMMC_CONTROL1, 2, true, 1000000)) { SetError("EMMC clock unstable"); return false; }
	write32(EMMC_CONTROL1, c | 5u);
	return true;
}
static bool R5Ok(u32 response)
{
	if (response & 0x0000CB00u) { SetError("SDIO R5 rejected command"); return false; }
	return true;
}
static bool Command(u32 index, u32 arg, u32 flags, u32* response)
{
	if (!WaitReg(EMMC_STATUS, 3, false, 100000)) { SetError("SDIO host busy"); return false; }
	write32(EMMC_INTERRUPT, 0xFFFFFFFF);
	write32(EMMC_ARG1, arg);
	write32(EMMC_CMDTM, (index << 24) | flags);
	if (!WaitReg(EMMC_INTERRUPT, 0x8001, true, 250000)) { SetError("SDIO command timeout"); return false; }
	u32 irq = read32(EMMC_INTERRUPT);
	// Do not acknowledge a simultaneously raised BUFFER_*_READY bit here.
	// CMD53 can present it with COMMAND_COMPLETE; clearing all observed bits
	// loses it and creates a false data timeout.
	write32(EMMC_INTERRUPT, 0xFFFF0001u);
	if (irq & 0xFFFF0000u) { SetError("SDIO command error"); return false; }
	if (response) *response = read32(EMMC_RESP0);
	return true;
}

bool WifiSdio_Cmd52(bool write, u8 function, u32 address, u8 writeValue, u8* readValue)
{
	if (function > 7 || address > 0x1FFFF) { SetError("CMD52 bad address"); return false; }
	u32 arg = (write ? 0x80000000u : 0) | ((u32)function << 28) | (address << 9) | writeValue;
	u32 response;
	if (!Command(52, arg, 0x000A0000u, &response) || !R5Ok(response)) return false;
	if (readValue) *readValue = (u8)response;
	return true;
}
bool WifiSdio_Cmd53(bool write, u8 function, u32 address, bool incrementingAddress, u8* buffer, u32 length)
{
	if (!buffer || !length || length > 508 || (length & 3) || function > 7 || address > 0x1FFFF)
	{ SetError("CMD53 invalid length/address"); return false; }
	if (!WaitReg(EMMC_STATUS, 3, false, 100000)) { SetError("CMD53 host busy"); return false; }
	write32(EMMC_INTERRUPT, 0xFFFFFFFF);
	write32(EMMC_BLKSIZECNT, length | (1u << 16));
	u32 arg = (write ? 0x80000000u : 0) | ((u32)function << 28) |
		(incrementingAddress ? (1u << 26) : 0) | (address << 9) | length;
	write32(EMMC_ARG1, arg);
	// One byte-mode block only. Bit 5 is MULTI_BLOCK and must stay clear:
	// setting it with a count of one made BCM43430 reject the CMD53 R5.
	write32(EMMC_CMDTM, (53u << 24) | 0x002A0000u | (write ? 0 : (1u << 4)));
	if (!WaitReg(EMMC_INTERRUPT, 0x8001, true, 250000)) { SetError("CMD53 command timeout"); ResetDataLine(); return false; }
	u32 irq = read32(EMMC_INTERRUPT);
	// Preserve a data-ready interrupt that may arrive with command complete.
	write32(EMMC_INTERRUPT, 0xFFFF0001u);
	u32 response = read32(EMMC_RESP0);
	if (irq & 0xFFFF0000u) { SetError("CMD53 host rejected"); ResetDataLine(); return false; }
	if (!R5Ok(response)) { ResetDataLine(); return false; }
	u32 ready = write ? (1u << 4) : (1u << 5);
	if (!WaitReg(EMMC_INTERRUPT, ready | 0x8000, true, 250000)) { SetError("CMD53 data timeout"); ResetDataLine(); return false; }
	irq = read32(EMMC_INTERRUPT); write32(EMMC_INTERRUPT, irq);
	if (irq & 0xFFFF0000u) { SetError("CMD53 data error"); ResetDataLine(); return false; }
	for (u32 i = 0; i < length; i += 4)
	{
		if (write) write32(EMMC_DATA, (u32)buffer[i] | ((u32)buffer[i+1] << 8) | ((u32)buffer[i+2] << 16) | ((u32)buffer[i+3] << 24));
		else { u32 v = read32(EMMC_DATA); buffer[i]=(u8)v; buffer[i+1]=(u8)(v>>8); buffer[i+2]=(u8)(v>>16); buffer[i+3]=(u8)(v>>24); }
	}
	if (!WaitReg(EMMC_INTERRUPT, 0x8002, true, 250000)) { SetError("CMD53 completion timeout"); ResetDataLine(); return false; }
	irq = read32(EMMC_INTERRUPT); write32(EMMC_INTERRUPT, irq);
	if (irq & 0xFFFF0000u) { SetError("CMD53 completion error"); ResetDataLine(); return false; }
	return true;
}
bool WifiSdio_EnableFunction(u8 function, u32 timeoutUs)
{
	if (function == 0 || function > 7) { SetError("invalid SDIO function"); return false; }
	u8 enabled;
	if (!WifiSdio_Cmd52(false, 0, 0x02, 0, &enabled) || !WifiSdio_Cmd52(true, 0, 0x02, (u8)(enabled | (1u << function)), 0)) return false;
	u32 deadline = read32(ARM_SYSTIMER_CLO) + timeoutUs;
	u8 ready = 0;
	while (!Expired(deadline)) { if (!WifiSdio_Cmd52(false, 0, 0x03, 0, &ready)) return false; if (ready & (1u << function)) return true; delay_us(100); }
	SetError("SDIO function enable timeout"); return false;
}
bool WifiSdio_SwitchToWlan()
{
#if !defined(RPI3)
	SetError("onboard WiFi requires Pi 3"); return false;
#else
	SetGroup(RPI_GPIO48, 6, FS_INPUT); SetGroup(RPI_GPIO34, 6, FS_ALT3); delay_us(10000); s_onWlan = true;
	if (!ResetController() || !SetClock(400000, true)) return false;
	write32(EMMC_INTERRUPT, 0xFFFFFFFF); write32(EMMC_IRPT_MASK, 0xFFFFFFFF); write32(EMMC_IRPT_EN, 0xFFFFFFFF);
	u32 response = 0;
	if (!s_everInitialized) { if (!Command(0, 0, 0, 0)) return false; }
	else { if (!WifiSdio_Cmd52(true, 0, 0x06, 8, 0)) return false; delay_us(1000); }
	if (!Command(5, 0, 0x00020000u, &response)) return false;
	u32 ocr = response & 0x00FFFFFFu, deadline = read32(ARM_SYSTIMER_CLO) + 1000000;
	do { if (!Command(5, ocr, 0x00020000u, &response)) return false; if (response & 0x80000000u) break; delay_us(10000); } while (!Expired(deadline));
	if (!(response & 0x80000000u)) { SetError("SDIO card not ready"); return false; }
	if (!Command(3, 0, 0x000A0000u, &response)) return false;
	s_rca = (u16)(response >> 16);
	if (!Command(7, (u32)s_rca << 16, 0x000A0000u, &response)) return false;
	if (!SetClock(25000000, false) || !WifiSdio_EnableFunction(1, 1000000)) return false;
	s_everInitialized = true; SetError(""); return true;
#endif
}
void WifiSdio_SwitchBackToSd() { SetGroup(RPI_GPIO34, 6, FS_INPUT); SetGroup(RPI_GPIO48, 6, FS_ALT3); s_onWlan = false; }
bool WifiSdio_IsOnWlanRoute() { return s_onWlan; }
const char* WifiSdio_LastError() { return s_error ? s_error : "not initialised"; }
u16 WifiSdio_Rca() { return s_rca; }
