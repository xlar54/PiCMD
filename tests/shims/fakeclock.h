// The fake microsecond clock, shared by the hardware shim (which serves it to
// the code under test through read32) and the filesystem shim (which advances
// it, so an SD access appears to take time).
//
// Kept separate from both so neither has to include the other.

#ifndef TESTS_FAKECLOCK_H
#define TESTS_FAKECLOCK_H

namespace FakeClock
{
	// Microseconds since "boot". Tests move this directly, and the filesystem
	// shim moves it on every access.
	extern unsigned int micros;

	// How long each SD access appears to take. The measured worst case on real
	// hardware is about 35000; tests that care about the flush budget set this
	// and then assert how much work fitted inside it.
	extern unsigned int microsPerAccess;

	void Reset();
	void Advance(unsigned int us);
}

#endif
