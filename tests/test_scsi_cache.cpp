// Tests for ScsiImage's write-back cache.
//
// Every case here corresponds to a bug that reached main and was found by
// reading rather than by running. The point is that they stay found.

#include "harness.h"
#include "shims/fakefs.h"
#include "shims/fakeclock.h"
#include "scsi.h"

#include <string.h>
#include <vector>

namespace
{
	const u32 SECTOR = ScsiImage::SECTOR_SIZE;					// 512
	const u32 PER_CHUNK = ScsiImage::SECTORS_PER_CHUNK;			// 8
	const char* IMAGE = "test.dhd";

	// Enough sectors to spill a 16 slot cache several times over.
	const u32 IMAGE_SECTORS = 1024;

	void FreshImage(unsigned char fill = 0xAB)
	{
		FakeFs::Reset();
		FakeClock::Reset();
		FakeFs::CreateFile(IMAGE, IMAGE_SECTORS * SECTOR, fill);
	}

	std::vector<u8> Pattern(unsigned char v)
	{
		return std::vector<u8>(SECTOR, v);
	}

	// What is actually on the card, as opposed to what the cache believes.
	unsigned char OnCard(u32 lba, u32 offsetInSector = 0)
	{
		// Name held in a local: passing the literal would build a temporary
		// std::string, and the reference handed back outlives it.
		const std::string name(IMAGE);
		const std::vector<unsigned char>& d = FakeFs::Contents(name);
		size_t at = (size_t)lba * SECTOR + offsetInSector;
		return at < d.size() ? d[at] : 0xFF;
	}
}

// ---------------------------------------------------------------------------

static void WriteThenFlushReachesTheCard()
{
	TEST_CASE("a flushed write reaches the card");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0x11);
	CHECK_EQ(img.WriteSector(3, &buf[0]), 0);

	// Nothing should have gone to the card yet - the write is acknowledged
	// out of RAM, which is the whole design.
	CHECK_EQ(OnCard(3), 0xAB);

	CHECK_EQ(img.Sync(true), 0);
	CHECK_EQ(OnCard(3), 0x11);
	CHECK_EQ(OnCard(3, SECTOR - 1), 0x11);

	// A second flush with nothing dirty must not touch the card.
	unsigned before = FakeFs::WriteCallCount();
	CHECK_EQ(img.Sync(true), 0);
	CHECK_EQ(FakeFs::WriteCallCount(), before);

	img.Detach();
}

// A full card reports FR_OK having written fewer bytes than asked. That used
// to count as success, the dirty bits were cleared, and the only copy of the
// data went away.
static void ShortWriteIsNotSuccess()
{
	TEST_CASE("a short write is a failure, and the data survives it");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0x22);
	CHECK_EQ(img.WriteSector(5, &buf[0]), 0);

	FakeFs::ShortWritesAfter(0, 16);		// next write stores 16 of 512 bytes
	CHECK(img.Sync(true) < 0);
	CHECK(img.HasWriteError());

	// The card has the truncated write on it, but the cache must still hold
	// the sector as dirty - so once the card recovers, a flush completes it.
	FakeFs::ShortWritesAfter(-1);
	CHECK_EQ(img.Sync(true), 0);
	CHECK_EQ(OnCard(5), 0x22);
	CHECK_EQ(OnCard(5, SECTOR - 1), 0x22);

	img.Detach();
}

static void HardWriteFailureKeepsTheData()
{
	TEST_CASE("a failed write keeps the data for a retry");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0x33);
	CHECK_EQ(img.WriteSector(7, &buf[0]), 0);

	FakeFs::FailWritesAfter(0);
	CHECK(img.Sync(true) < 0);
	CHECK_EQ(OnCard(7), 0xAB);				// nothing landed

	// Reading it back must give the new data - it is still in the cache.
	std::vector<u8> got(SECTOR, 0);
	CHECK_EQ(img.ReadSector(7, &got[0]), 0);
	CHECK_EQ(got[0], 0x33);

	FakeFs::FailWritesAfter(-1);
	CHECK_EQ(img.Sync(true), 0);
	CHECK_EQ(OnCard(7), 0x33);

	img.Detach();
}

// ReadSectorUncached checked the slot's valid flag but not the requested
// sector's validMask bit, so a chunk created by writing one sector handed back
// uninitialised RAM for the other seven - to the base LBA scan, which is what
// decides where the CMD signature is.
static void UncachedReadDoesNotInventData()
{
	TEST_CASE("an uncached read of an unwritten sector comes from the card");
	FreshImage(0xAB);

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	// Write sector 0 of chunk 0. Sectors 1..7 of that chunk are now present
	// in the slot but hold nothing meaningful.
	std::vector<u8> buf = Pattern(0x44);
	CHECK_EQ(img.WriteSector(0, &buf[0]), 0);

	for (u32 s = 1; s < PER_CHUNK; ++s)
	{
		std::vector<u8> got(SECTOR, 0);
		CHECK_EQ(img.ReadSectorUncached(s, &got[0]), 0);
		CHECK_MSG(got[0] == 0xAB, "unwritten sector should read as what is on the card");
	}

	// The written one still comes back from the cache.
	std::vector<u8> got(SECTOR, 0);
	CHECK_EQ(img.ReadSectorUncached(0, &got[0]), 0);
	CHECK_EQ(got[0], 0x44);

	img.Detach();
}

// The budget is what keeps a reset from taking the drive off the bus for
// minutes. It was counted in chunks, which bounds the work only to within the
// number of separate writes a chunk needs.
static void FlushBudgetIsBounded()
{
	TEST_CASE("a budgeted flush stops near its deadline");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	// Dirty a lot of separate chunks.
	std::vector<u8> buf = Pattern(0x55);
	for (u32 c = 0; c < 40; ++c)
		CHECK_EQ(img.WriteSector(c * PER_CHUNK, &buf[0]), 0);

	// Make each access cost the worst time measured on real hardware.
	FakeClock::microsPerAccess = 35000;

	u32 start = FakeClock::micros;
	ScsiImage::FlushSome(250000);
	u32 took = FakeClock::micros - start;

	// Soft budget: it stops starting new work after the deadline, and a write
	// already under way cannot be abandoned. One access of overrun is
	// expected; ten would mean it is not bounded at all.
	CHECK_MSG(took <= 250000 + 2 * 35000, "flush overran its budget by more than one access");
	CHECK_MSG(took >= 100000, "flush gave up far too early to be doing real work");

	// And it must not have marked itself clean - there is more to write.
	unsigned before = FakeFs::WriteCallCount();
	FakeClock::microsPerAccess = 0;
	ScsiImage::FlushAll();
	CHECK_MSG(FakeFs::WriteCallCount() > before, "the rest of the dirty chunks were never written");

	img.Detach();
}

static void UnbudgetedFlushFinishesAndMarksClean()
{
	TEST_CASE("an unbudgeted flush finishes and marks the cache clean");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0x66);
	for (u32 c = 0; c < 12; ++c)
		CHECK_EQ(img.WriteSector(c * PER_CHUNK, &buf[0]), 0);

	CHECK_EQ(ScsiImage::FlushAll(), 0);

	for (u32 c = 0; c < 12; ++c)
		CHECK_EQ(OnCard(c * PER_CHUNK), 0x66);

	// Clean now: another flush must do no work at all.
	unsigned before = FakeFs::WriteCallCount();
	CHECK_EQ(ScsiImage::FlushAll(), 0);
	CHECK_EQ(FakeFs::WriteCallCount(), before);

	img.Detach();
}

// With the cache full and the card refusing writes, a chunk that cannot be
// written must not be evicted - the cache is the only copy. WriteSector then
// falls back to writing straight through and reports the failure honestly.
static void EvictionDoesNotDiscardUnwritableData()
{
	TEST_CASE("eviction does not discard what it cannot write");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0x77);

	// Fill the cache with dirty chunks.
	for (u32 c = 0; c < 16; ++c)
		CHECK_EQ(img.WriteSector(c * PER_CHUNK, &buf[0]), 0);

	// Now break the card and keep writing, forcing evictions that cannot
	// succeed. Some of these will fail, which is correct - what must not
	// happen is a silent success that loses data.
	FakeFs::FailWritesAfter(0);
	int reported = 0;
	for (u32 c = 16; c < 64; ++c)
	{
		if (img.WriteSector(c * PER_CHUNK, &buf[0]) != 0)
			++reported;
	}
	CHECK_MSG(reported > 0, "writes against a dead card all claimed to succeed");

	// Recover the card. Everything still held dirty must now land.
	FakeFs::FailWritesAfter(-1);
	ScsiImage::FlushAll();

	img.Detach();
}

static void DetachOnADeadCardDoesNotPretend()
{
	TEST_CASE("detach on a dead card reports rather than pretending");
	FreshImage();

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0x88);
	CHECK_EQ(img.WriteSector(9, &buf[0]), 0);

	u32 errorsBefore = ScsiImage::WriteErrorCount();
	FakeFs::FailWritesAfter(0);
	img.Detach();								// must not hang or crash

	CHECK_MSG(ScsiImage::WriteErrorCount() > errorsBefore,
		"a detach that lost data did not count an error");
	CHECK_EQ(OnCard(9), 0xAB);					// nothing landed, as expected
}

// ---------------------------------------------------------------------------

void RunScsiCacheTests()
{
	// One shot, and small so eviction is reachable: 64KB is 16 chunks.
	ScsiImage::InitCache(64 * 1024);

	WriteThenFlushReachesTheCard();
	ShortWriteIsNotSuccess();
	HardWriteFailureKeepsTheData();
	UncachedReadDoesNotInventData();
	FlushBudgetIsBounded();
	UnbudgetedFlushFinishesAndMarksClean();
	EvictionDoesNotDiscardUnwritableData();
	DetachOnADeadCardDoesNotPretend();
}
