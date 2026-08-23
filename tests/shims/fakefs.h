// Test side control over the fake filesystem in ff.cpp.
//
// The emulator only ever sees ff.h; this is how a test sets a card up, breaks
// it, and then inspects what actually landed on it.

#ifndef TESTS_FAKEFS_H
#define TESTS_FAKEFS_H

#include <stddef.h>
#include <vector>
#include <string>

namespace FakeFs
{
	// Forget every file and clear all injected faults. Call at the top of each
	// test - the fake filesystem is global, like the real one.
	void Reset();

	// Create a file of `size` bytes, filled with `fill`.
	void CreateFile(const std::string& name, size_t size, unsigned char fill = 0);

	// What the "card" holds right now. This is the ground truth a test asserts
	// against - not what the cache thinks it wrote.
	const std::vector<unsigned char>& Contents(const std::string& name);

	bool Exists(const std::string& name);

	// ---- fault injection -------------------------------------------------
	//
	// Counts are "let this many succeed, then start failing". -1 disables.

	// Return FR_DISK_ERR from f_write.
	void FailWritesAfter(int successes);

	// Return FR_OK from f_write but report fewer bytes than asked - how a full
	// card behaves, and the case that used to be taken for success.
	void ShortWritesAfter(int successes, unsigned bytesToActuallyWrite = 0);

	void FailReadsAfter(int successes);
	void FailSync(bool fail);
	void FailLseek(bool fail);

	// Counters, for asserting that a flush did or did not go to the card.
	unsigned WriteCallCount();
	unsigned ReadCallCount();
	unsigned SyncCallCount();
}

#endif
