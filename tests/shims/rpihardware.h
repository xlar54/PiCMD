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

// The value only has to be distinguishable, not real.
#define ARM_SYSTIMER_CLO	0x3F003004u

static inline unsigned int read32(unsigned int nAddress)
{
	if (nAddress == ARM_SYSTIMER_CLO)
		return FakeClock::micros;
	return 0;
}

static inline void write32(unsigned int nAddress, unsigned int nValue)
{
	(void)nAddress;
	(void)nValue;
}

#endif
