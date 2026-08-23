// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// SCSI disk (target) emulation.
// Ported from VICE's scsi.c/scsi.h written by Roberto Muscedere, adapted to
// FatFS backed image files with a write-through sector cache.
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

#ifndef SCSI_H
#define SCSI_H

#include "types.h"
#include "ff.h"

struct scsi_context_s;

// A SCSI hard disk image backed by a FatFS file.
//
// Accesses go through a write-back cache of CACHE_CHUNK_SIZE chunks so that
// most of them do not touch the SD card. That matters more than throughput:
// an SD access blocks the emulated CPU inside a single instruction, and a
// frozen CPU cannot answer ATN, so the computer decides the drive is not
// there. Writes used to go straight through, which meant every 512 byte
// sector stalled the drive - and HD-TOOLS writes the system area one sector
// per SCSI command, polling for status in between.
class ScsiImage
{
public:
	static const u32 SECTOR_SIZE = 512;
	static const u32 CACHE_CHUNK_SHIFT = 12;			// 4K chunks (8 sectors)
	static const u32 CACHE_CHUNK_SIZE = 1 << CACHE_CHUNK_SHIFT;
	static const u32 SECTORS_PER_CHUNK = CACHE_CHUNK_SIZE / SECTOR_SIZE;

	ScsiImage() : attached(false), sizeInSectors(0), readOnly(false), needSync(false), writeErrorPending(false) {}

	bool Attach(const char* filename, bool readOnly);
	void Detach();
	bool IsAttached() const { return attached; }
	u32 SizeInSectors() const { return sizeInSectors; }
	bool IsReadOnly() const { return readOnly; }
	const char* GetFileName() const { return name; }

	// Read/write one 512 byte sector. Returns 0 on success.
	int ReadSector(u32 lba, u8* buffer);
	// Read one sector without disturbing (or filling) the cache. The whole
	// disk scans step 128 sectors at a time, so a 4K chunk fetch would throw
	// away seven eighths of what it reads and evict something useful.
	int ReadSectorUncached(u32 lba, u8* buffer);
	int WriteSector(u32 lba, const u8* buffer);

	// Longest single SD access so far, in microseconds. The bus timeout that
	// shows up as ?DEVICE NOT PRESENT is a few milliseconds, so this is the
	// number that says whether the cache is doing its job.
	static u32 WorstStallMicros() { return worstStallMicros; }
	static void ResetWorstStall() { worstStallMicros = 0; }

	// Bumped on every access, so the caller can tell whether the disk has been
	// left alone.
	static u32 AccessCounter() { return accessCounter; }

	// Flush every attached image. Returns the number of images that could not
	// be written. Only call this when the serial bus is quiet: it goes to the
	// card, which freezes the emulated CPU, and a frozen CPU cannot answer
	// ATN. Reset is also a safe moment - the drive is restarting anyway, and
	// losing acknowledged writes there is worse than a pause.
	static int FlushAll();

	// As FlushAll, but stops after roughly maxChunks chunks (0 = no limit).
	// Reset uses this: that path is reached from the IEC RESET line with the
	// host already running, so an unbounded flush would take the drive off the
	// bus for as long as the card needs - minutes, with a large dirty cache.
	static int FlushSome(u32 maxChunks);

	// About a quarter second of card time at the worst measured rate.
	static const u32 RESET_FLUSH_CHUNKS = 8;

	// Flush dirty chunks and FatFS metadata. Does nothing unless forced -
	// going to the card at an arbitrary moment is what broke the bus. Detach
	// and reset force it; everything else waits for an idle window.
	// Returns 0 if everything went out, 1 if the chunk budget stopped it part
	// way (more still dirty), negative on a write failure. sharedFlushed lets
	// several images draw on one budget; pass 0 for a private one.
	int Sync(bool force = false, u32 maxChunks = 0, u32* sharedFlushed = 0);

	// Writes are acknowledged to the host as soon as they reach the cache, so
	// by the time a flush fails the computer has long since been told the
	// write succeeded. There is no way to un-acknowledge it, so instead the
	// data stays dirty for a later retry and this latches - the next command
	// for the image reports CHECK CONDITION / MEDIUM ERROR so the failure
	// surfaces as a drive error rather than as silent corruption.
	bool HasWriteError() const { return writeErrorPending; }
	bool TakeWriteError() { bool e = writeErrorPending; writeErrorPending = false; return e; }

	// Total flush failures since boot, for the display and for bug reports.
	static u32 WriteErrorCount() { return writeErrors; }

	// The (global) cache used by all images.
	static void InitCache(u32 sizeInBytes);

private:
	FIL file;
	bool attached;
	u32 sizeInSectors;
	bool readOnly;
	bool needSync;
	bool writeErrorPending;
	static u32 worstStallMicros;
	static u32 accessCounter;
	static u32 writeErrors;

	char name[256];

	struct CacheSlot
	{
		u8* data;
		u32 chunkIndex;
		u8 image;		// which image (index into a small registry)
		u8 valid;		// slot is allocated to this chunk
		// Per sector, because a write must never have to read first. Reading
		// the chunk in just to modify one sector of it meant an SD access on
		// the first write to every chunk, and an SD access freezes the drive
		// for as long as the card feels like - 38ms, measured. The computer
		// gives up on us long before that.
		u8 validMask;	// sectors holding real data
		u8 dirtyMask;	// sectors not yet on the card
	};

	// Push one dirty chunk out to the card. Used when a slot is reused for
	// something else, and by Sync.
	// Both return 0 on success. A chunk that fails keeps its dirty bits so the
	// next flush tries again rather than dropping the data on the floor.
	int FlushChunk(CacheSlot& slot);
	int FlushAllDirty(u32 maxChunks = 0, u32* flushed = 0);

	// Write a slot back so it can be reused. False means the data could not be
	// written and the slot must be left where it is - the cache is the only
	// copy, so evicting it anyway is data loss.
	static bool EvictSlot(CacheSlot* victim);
	static void NoteStall(u32 startMicros);

	// A chunk being evicted can belong to a different disk, so we need to get
	// back from the id stored in the slot to the image that owns the file.
	static ScsiImage* attachedImages[64];
	static u32 numAttachedImages;
	static ScsiImage* ImageById(u8 id);

	static u8* cachePool;
	static CacheSlot* cacheSlots;
	static u32 numCacheSlots;
	static u8 nextImageId;

	u8 imageId;

	int FillChunk(u32 chunkIndex, CacheSlot& slot);
	CacheSlot* FindSlot(u32 chunkIndex, bool allocate);
};

// SCSI bus states as presented by U13 to the CMD HD (see cmdhd glue).
#define SCSI_STATE_DATAOUT    0x00
#define SCSI_STATE_DATAIN     0x01
#define SCSI_STATE_COMMAND    0x02
#define SCSI_STATE_STATUS     0x03
#define SCSI_STATE_MESSAGEOUT 0x04
#define SCSI_STATE_MESSAGEIN  0x05
#define SCSI_STATE_BUSFREE    0x10
#define SCSI_STATE_SELECTION  0x20

#define SCSI_STATUS_GOOD                     0x00
#define SCSI_STATUS_CHECKCONDITION           0x01

#define SCSI_SENSEKEY_NOSENSE        0x00
#define SCSI_SENSEKEY_MEDIUMERROR    0x03
#define SCSI_SENSEKEY_ILLEGALREQUEST 0x05

#define SCSI_SASC_LOGICALBLOCKADDRESSOUTOFRANGE 0x21
#define SCSI_SASC_INVALIDFIELDINCDB             0x24
#define SCSI_SASC_INVALIDFIELDINPARAMETERLIST   0x26

#define SCSI_LOG_NODISK0 0x01

#define SCSI_COMMAND_TEST_UNIT_READY       0x00
#define SCSI_COMMAND_REZERO_UNIT           0x01
#define SCSI_COMMAND_REQUEST_SENSE         0x03
#define SCSI_COMMAND_FORMAT_UNIT           0x04
#define SCSI_COMMAND_REASSIGN_BLOCKS       0x07
#define SCSI_COMMAND_READ_6                0x08
#define SCSI_COMMAND_WRITE_6               0x0a
#define SCSI_COMMAND_INQUIRY               0x12
#define SCSI_COMMAND_MODE_SENSE            0x1a
#define SCSI_COMMAND_START_STOP            0x1b
#define SCSI_COMMAND_SEND_DIAGNOSTIC       0x1d
#define SCSI_COMMAND_READ_CAPACITY         0x25
#define SCSI_COMMAND_READ_10               0x28
#define SCSI_COMMAND_WRITE_10              0x2a
#define SCSI_COMMAND_VERIFY                0x2f
#define SCSI_COMMAND_MODE_SENSE_10         0x5a
#define SCSI_COMMAND_WRITE_VERIFY          0x2e

#define SCSI_MAX_DISKS 56

typedef struct scsi_context_s
{
	u8 state;
	u8 target;
	u8 databus;
	u8 ack;
	u8 req;
	u8 bsyi;
	u8 bsyo;
	u8 sel;
	u8 rst;
	u8 atn;
	u8 cd;
	u8 io;
	u8 msg;
	u32 seq;
	u32 cmd_size;
	u32 address;
	u32 blocks;
	u32 data_max;
	u8 link;
	u8 status;
	u8 lun;
	u8 command;
	u8 sensekey;
	u8 asc;
	u8 cmd_buf[256];
	u8 data_buf[512];
	u8 msg_after_status;
	u32 max_imagesize;		/* in 512 byte sectors; 0 = use image length */
	u32 limit_imagesize;	/* in 512 byte sectors */
	u32 log;
	ScsiImage* file[SCSI_MAX_DISKS];
	void* p;
	void (*user_format)(struct scsi_context_s*);
	void (*user_read)(struct scsi_context_s*);
	void (*user_write)(struct scsi_context_s*);
} scsi_context_t;

s32 scsi_image_read(scsi_context_t* context);
s32 scsi_image_read_uncached(scsi_context_t* context);
s32 scsi_image_write(scsi_context_t* context);
u8 scsi_get_bus(scsi_context_t* context);
int scsi_set_bus(scsi_context_t* context, u8 value);
void scsi_process_noack(scsi_context_t* context);
void scsi_process_ack(scsi_context_t* context);
void scsi_reset(scsi_context_t* context);

#endif
