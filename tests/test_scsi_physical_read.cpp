// Tests for ReadSectorPhysical - the read path VERIFY uses.
//
// VERIFY exists to find bad media. Reading through the cache verifies the
// cache: a sector written a moment ago, or read earlier and still resident,
// comes back perfectly from a card that has since been pulled. So the property
// under test is not "it returns the right bytes" but "it goes to the card,
// every time, and notices when the card will not answer".

#include "harness.h"
#include "shims/fakefs.h"
#include "shims/fakeclock.h"
#include "scsi.h"

#include <string.h>
#include <vector>

namespace
{
	const u32 SECTOR = ScsiImage::SECTOR_SIZE;
	const char* IMAGE = "verify.dhd";
	const u32 IMAGE_SECTORS = 256;

	void FreshImage(unsigned char fill)
	{
		FakeFs::Reset();
		FakeClock::Reset();
		FakeFs::CreateFile(IMAGE, IMAGE_SECTORS * SECTOR, fill);
	}

	std::vector<u8> Pattern(unsigned char v)
	{
		return std::vector<u8>(SECTOR, v);
	}
}

// The distinguishing case. A sector is written (so the cache holds it, dirty
// and unflushed) and then read physically - which must return what is on the
// card, not what the cache is holding. Through the cache these are the same
// bytes and the test would prove nothing; unflushed, they differ.
static void PhysicalReadIgnoresDirtyCache()
{
	TEST_CASE("a physical read returns the card, not unflushed cache contents");
	FreshImage(0xC1);

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> buf = Pattern(0xC2);
	CHECK_EQ(img.WriteSector(4, &buf[0]), 0);		// dirty, not flushed

	std::vector<u8> got(SECTOR, 0);
	CHECK_EQ(img.ReadSectorPhysical(4, &got[0]), 0);
	CHECK_MSG(got[0] == 0xC1, "physical read was served from the cache");

	// Through the ordinary path it is the cached value, as it should be.
	std::vector<u8> cached(SECTOR, 0);
	CHECK_EQ(img.ReadSector(4, &cached[0]), 0);
	CHECK_EQ(cached[0], 0xC2);

	// And once flushed, the two agree.
	CHECK_EQ(img.Sync(true), 0);
	CHECK_EQ(img.ReadSectorPhysical(4, &got[0]), 0);
	CHECK_EQ(got[0], 0xC2);

	img.Detach();
}

// A sector that has been read once sits in the cache. If the card then stops
// answering, a verify must notice - which it cannot if the cache satisfies the
// read.
static void PhysicalReadNoticesADeadCard()
{
	TEST_CASE("a physical read fails on a dead card even when cached");
	FreshImage(0xD1);

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	// Pull it into the cache the ordinary way.
	std::vector<u8> got(SECTOR, 0);
	CHECK_EQ(img.ReadSector(6, &got[0]), 0);
	CHECK_EQ(got[0], 0xD1);

	// Now the card stops answering.
	FakeFs::FailReadsAfter(0);

	// The cached path can still satisfy this - that is the cache doing its
	// job, and is not what VERIFY should be asking.
	CHECK_EQ(img.ReadSector(6, &got[0]), 0);

	// The physical path must not.
	CHECK_MSG(img.ReadSectorPhysical(6, &got[0]) != 0,
		"physical read succeeded against a card that cannot be read");

	FakeFs::FailReadsAfter(-1);
	img.Detach();
}

static void PhysicalReadAlwaysTouchesTheCard()
{
	TEST_CASE("a physical read goes to the card every time");
	FreshImage(0xE1);

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> got(SECTOR, 0);

	unsigned before = FakeFs::ReadCallCount();
	CHECK_EQ(img.ReadSectorPhysical(9, &got[0]), 0);
	unsigned afterFirst = FakeFs::ReadCallCount();
	CHECK_MSG(afterFirst > before, "first physical read did not reach the card");

	// A second read of the same sector must go again - no caching, not even
	// opportunistically.
	CHECK_EQ(img.ReadSectorPhysical(9, &got[0]), 0);
	CHECK_MSG(FakeFs::ReadCallCount() > afterFirst,
		"second physical read was served from somewhere other than the card");

	img.Detach();
}

static void PhysicalReadRejectsOutOfRange()
{
	TEST_CASE("a physical read past the end of the image fails");
	FreshImage(0xF1);

	ScsiImage img;
	CHECK(img.Attach(IMAGE, false));

	std::vector<u8> got(SECTOR, 0);
	CHECK_EQ(img.ReadSectorPhysical(IMAGE_SECTORS - 1, &got[0]), 0);

	// Unlike ReadSector, which zero fills past the end to match VICE, a verify
	// of a block that is not on the disk is an error rather than a hole.
	CHECK_MSG(img.ReadSectorPhysical(IMAGE_SECTORS, &got[0]) != 0,
		"physical read past the end reported success");
	CHECK_MSG(img.ReadSectorPhysical(IMAGE_SECTORS + 1000, &got[0]) != 0,
		"physical read well past the end reported success");

	img.Detach();
}

static void PhysicalReadOnADetachedImageFails()
{
	TEST_CASE("a physical read on a detached image fails");
	FreshImage(0x01);

	ScsiImage img;
	std::vector<u8> got(SECTOR, 0);
	CHECK_MSG(img.ReadSectorPhysical(0, &got[0]) != 0,
		"physical read succeeded with nothing attached");
}

// ---------------------------------------------------------------------------

void RunScsiPhysicalReadTests()
{
	PhysicalReadIgnoresDirtyCache();
	PhysicalReadNoticesADeadCard();
	PhysicalReadAlwaysTouchesTheCard();
	PhysicalReadRejectsOutOfRange();
	PhysicalReadOnADetachedImageFails();
}
