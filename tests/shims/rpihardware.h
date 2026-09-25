// Host side stand-in for the Pi's MMIO accessors.
//
// The disk cache reaches hardware through exactly two things: read32 and the
// ARM_SYSTIMER_CLO address. Faking them lets a test drive the clock by hand,
// which is the only way to test a flush budget expressed in microseconds
// without sleeping - and sleeping in a test is both slow and flaky.
//
// Shadows src/rpi/rpihardware.h by being earlier on the include path. The
// emulator source is compiled unmodified against it.

#ifndef TESTS_SHIM_RPIHARDWARE_H
#define TESTS_SHIM_RPIHARDWARE_H

#include "fakeclock.h"
#include "fakegpio.h"

// GPIO addresses used by the actual IEC implementation. Hardware accesses
// are captured by fakegpio; the real GPIO header still supplies pin enums.
#define ARM_GPIO_GPFSEL0 0x20200000u
#define ARM_GPIO_GPFSEL1 0x20200004u
#define ARM_GPIO_GPSET0 0x2020001cu
#define ARM_GPIO_GPCLR0 0x20200028u
#define ARM_GPIO_GPLEV0 0x20200034u
#define CM_PWMDIV 0x201010a4u
#define CM_PWMCTL 0x201010a0u
#define CM_PASSWORD 0x5a000000u
#define CM_ENAB 0x10u
#define CM_SRC_OSCILLATOR 1u
#define PWM_RNG1 0x2020c010u
#define PWM_RNG2 0x2020c020u
#define PWM_CTL 0x2020c000u
#define PWM_USEF2 0x2000u
#define PWM_PWEN2 0x100u
#define PWM_USEF1 0x20u
#define PWM_PWEN1 1u
#define PWM_CLRF1 0x40u

// The value only has to be distinguishable, not real.
#define ARM_SYSTIMER_CLO	0x3F003004u

static inline unsigned int read32(unsigned int nAddress)
{
	if (nAddress == ARM_SYSTIMER_CLO)
		return FakeClock::micros;
	return FakeGPIO::Read(nAddress);
}

static inline void write32(unsigned int nAddress, unsigned int nValue)
{
	FakeGPIO::Write(nAddress, nValue);
}

#endif
