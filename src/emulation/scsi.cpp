// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// SCSI disk (target) emulation.
// Ported from VICE's scsi.c written by Roberto Muscedere, adapted to
// FatFS backed image files with a write-back sector cache.
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "scsi.h"
#include "debug.h"
#include "rpihardware.h"

#define MAXIDS 7
#define MAXLUNS 8

///////////////////////////////////////////////////////////////////////////////
// ScsiImage - a FatFS backed hard disk image with a write-back cache.
///////////////////////////////////////////////////////////////////////////////

u8* ScsiImage::cachePool = 0;
ScsiImage::CacheSlot* ScsiImage::cacheSlots = 0;
u32 ScsiImage::numCacheSlots = 0;

void ScsiImage::InitCache(u32 sizeInBytes)
{
	if (cachePool)
		return;

	u32 slots = sizeInBytes >> CACHE_CHUNK_SHIFT;
	if (slots < 16)
		slots = 16;

	// Settle for a smaller cache rather than none: with numCacheSlots at zero
	// every single access goes to the card synchronously, which is exactly the
	// stall the cache exists to avoid.
	for (;;)
	{
		cachePool = (u8*)malloc(slots * CACHE_CHUNK_SIZE);
		cacheSlots = (CacheSlot*)malloc(slots * sizeof(CacheSlot));
		if (cachePool && cacheSlots)
			break;

		free(cachePool);
		free(cacheSlots);
		cachePool = 0;
		cacheSlots = 0;

		if (slots <= 16)
		{
			DEBUG_LOG("SCSI: failed to allocate %u byte cache\r\n", slots * CACHE_CHUNK_SIZE);
			numCacheSlots = 0;
			return;
		}

		slots >>= 1;
		if (slots < 16)
			slots = 16;
	}

	numCacheSlots = slots;

	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		cacheSlots[i].data = cachePool + (i * CACHE_CHUNK_SIZE);
		cacheSlots[i].valid = 0;
	}
	DEBUG_LOG("SCSI: %u KB disk cache (%u chunks)\r\n", (numCacheSlots * CACHE_CHUNK_SIZE) >> 10, numCacheSlots);
}

bool ScsiImage::Attach(const char* filename, bool readOnly)
{
	Detach();

	BYTE mode = readOnly ? FA_READ : (FA_READ | FA_WRITE);
	if (f_open(&file, filename, mode) != FR_OK)
	{
		// Retry read-only; the file may have the read only attribute set.
		if (!readOnly && f_open(&file, filename, FA_READ) == FR_OK)
			readOnly = true;
		else
			return false;
	}

	this->readOnly = readOnly;
	sizeInSectors = (u32)((f_size(&file) + SECTOR_SIZE - 1) >> 9);
	strncpy(name, filename, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	attached = true;
	needSync = false;
	// Take the lowest id no attached image is using, rather than counting up
	// forever: the id is only 8 bits and it picks cache slots, so once it wraps
	// two live images would answer to the same one and read each other's
	// chunks - or get flushed through the wrong file handle. At most
	// SCSI_MAX_DISKS are ever attached, so a free id always exists.
	imageId = 0;
	for (u32 i = 0; i < numAttachedImages; )
	{
		if (attachedImages[i]->imageId == imageId)
		{
			imageId++;
			i = 0;		// start over; the new id may collide with an earlier one
		}
		else
		{
			i++;
		}
	}
	if (numAttachedImages < 64)
		attachedImages[numAttachedImages++] = this;

	// Invalidate any stale cache entries carrying this image id.
	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		if (cacheSlots[i].valid && cacheSlots[i].image == imageId)
			cacheSlots[i].valid = 0;
	}

	DEBUG_LOG("SCSI: attached %s (%u sectors)%s\r\n", filename, sizeInSectors, readOnly ? " R/O" : "");
	return true;
}

void ScsiImage::Detach()
{
	if (attached)
	{
		// Retry before giving up. Below, every slot belonging to this image is
		// invalidated, so whatever is still dirty at that point is gone - and
		// this is the last chance to notice. A transient write error is worth
		// a second attempt; a full card is not going to improve, but it is
		// going to be reported instead of swallowed.
		for (u32 attempt = 0; attempt < 3 && !Sync(true); ++attempt)
			;
		if (needSync)
			unflushedSectors += DirtySectorCount();

		f_close(&file);
		attached = false;

		for (u32 i = 0; i < numAttachedImages; ++i)
		{
			if (attachedImages[i] == this)
			{
				attachedImages[i] = attachedImages[--numAttachedImages];
				attachedImages[numAttachedImages] = 0;
				break;
			}
		}
		sizeInSectors = 0;

		// Release the pinned region before invalidating, or the slots it covers
		// would stay reserved for an image that is no longer here.
		if (pinnedChunks && pinnedImage == imageId)
			pinnedChunks = 0;

		for (u32 i = 0; i < numCacheSlots; ++i)
		{
			if (cacheSlots[i].valid && cacheSlots[i].image == imageId)
				cacheSlots[i].valid = 0;
		}
	}
}

bool ScsiImage::Sync(bool force)
{
	if (!attached || !needSync)
		return true;

	// Only ever flush on demand. This used to run on a timer from the SCSI
	// state machine returning to BUSFREE - which is between commands, exactly
	// when the computer is about to poll for status. A single SD access has
	// been measured at 35ms on this hardware, far longer than the bus will
	// wait, so the drive vanished mid-transfer. Flushing now happens only when
	// the bus has gone quiet (FlushIdle) or on detach.
	if (!force)
		return true;

	// needSync stays set when this fails, so the next quiet moment - or the
	// next Detach - tries again rather than the data quietly ceasing to exist.
	// No bound here: detach has to finish, and by then the bus is done with us.
	if (FlushAllDirty(0, 0) != FLUSH_DONE)
		return false;

	f_sync(&file);
	needSync = false;
	return true;
}

// Sectors still marked dirty across every slot of this image: what would be
// thrown away if the slots were invalidated right now.
u32 ScsiImage::DirtySectorCount()
{
	u32 n = 0;
	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		if (!(cacheSlots[i].valid && cacheSlots[i].image == imageId))
			continue;
		u8 m = cacheSlots[i].dirtyMask;
		while (m) { n += (m & 1); m >>= 1; }
	}
	return n;
}

ScsiImage::CacheSlot* ScsiImage::FindSlot(u32 chunkIndex, bool allocate)
{
	if (!cacheSlots)
		return 0;

	// Pinned region. Chunk N of the preloaded image lives in slot N and nowhere
	// else, so nothing can ever evict it and no access to it can reach the
	// card. A hash would not do: 4096 chunks scattered over 16384 slots two
	// ways deep still collide often enough to leave a few hundred holes, and
	// one hole in the wrong place is the whole bug.
	if (pinnedChunks && imageId == pinnedImage && chunkIndex < pinnedChunks)
		return &cacheSlots[chunkIndex];

	// 2-way set associative on a multiplicative hash, over whatever is left.
	u32 span = numCacheSlots - pinnedChunks;
	if (!span)
		return 0;
	u32 hash = pinnedChunks + (chunkIndex * 2654435761u + imageId * 40503u) % span;
	u32 hash2 = pinnedChunks + (hash + 1 - pinnedChunks) % span;

	CacheSlot* slot = &cacheSlots[hash];
	if (slot->valid && slot->image == imageId && slot->chunkIndex == chunkIndex)
		return slot;
	CacheSlot* slot2 = &cacheSlots[hash2];
	if (slot2->valid && slot2->image == imageId && slot2->chunkIndex == chunkIndex)
		return slot2;

	if (!allocate)
		return 0;

	// Prefer an unused slot, then a clean one. Evicting a dirty chunk means a
	// synchronous write to the card from whatever called us - including
	// WriteSector, which must not go near it while the computer is polling for
	// status. Only pick a dirty victim when the whole set is dirty.
	CacheSlot* victim;
	if (!slot->valid)
		victim = slot;
	else if (!slot2->valid)
		victim = slot2;
	else if (!slot->dirtyMask)
		victim = slot;
	else
		victim = slot2;

	// Never drop writes on the floor. The chunk being evicted may belong to
	// another image, so flush it through whoever owns it.
	//
	// This is the one path that can still put an SD write in the middle of a
	// WriteSector, contradicting the promise made there. It only happens when
	// every way of the set is dirty, which in turn only happens when FlushIdle
	// has not had a quiet moment to run - exactly the case during a long
	// transfer. Count it so it stops being guesswork.
	if (victim->valid && victim->dirtyMask)
	{
		dirtyEvictions++;
		ScsiImage* owner = ImageById(victim->image);
		if (!owner || owner->FlushChunk(*victim) != 0)
		{
			// The card would not take it. Leave the chunk dirty so a later
			// flush can try again and hand the caller nothing, which sends it
			// straight to the file - losing the sectors here would be silent.
			return 0;
		}
	}

	victim->valid = 0;
	victim->validMask = 0;
	victim->dirtyMask = 0;
	victim->image = imageId;
	victim->chunkIndex = chunkIndex;
	return victim;
}

ScsiImage* ScsiImage::attachedImages[64] = { 0 };
u32 ScsiImage::numAttachedImages = 0;
u32 ScsiImage::worstStallMicros = 0;
u32 ScsiImage::accessCounter = 0;
u32 ScsiImage::dirtyEvictions = 0;
u32 ScsiImage::cacheReadHits = 0;
u32 ScsiImage::cacheReadMisses = 0;
u32 ScsiImage::dirtyChunkReads = 0;
u32 ScsiImage::dirtyChunkPartialFills = 0;

#if PICMD_ACCESS_FORENSICS
ScsiImage::AccessLogEntry ScsiImage::accessLog[ScsiImage::ACCESS_LOG_ENTRIES];
u32 ScsiImage::accessLogCount = 0;
#endif
u32 ScsiImage::maxLbaWritten = 0;

void ScsiImage::ResetCounters()
{
	worstStallMicros = 0;
	dirtyEvictions = 0;
	cacheReadHits = 0;
	cacheReadMisses = 0;
	dirtyChunkReads = 0;
	dirtyChunkPartialFills = 0;
	bitRotWrites = 0;
	bitRotBytes = 0;
	bitRotFirstLba = 0;
	bitRotMask = 0;
#if PICMD_ACCESS_FORENSICS
	accessLogCount = 0;
#endif
	maxLbaWritten = 0;
#if PICMD_ACCESS_FORENSICS
	writeLogCount = 0;
#endif
}

u32 ScsiImage::unflushedSectors = 0;
u32 ScsiImage::bitRotWrites = 0;
u32 ScsiImage::bitRotBytes = 0;
u32 ScsiImage::bitRotFirstLba = 0;
u8 ScsiImage::bitRotMask = 0;

u32 ScsiImage::pinnedChunks = 0;
u8 ScsiImage::pinnedImage = 0;

// Read the front of the image into slots 0..n-1 and pin it there. Called at
// mount, with the bus quiet and nobody waiting on us, so the cost of going to
// the card is paid once instead of arriving in the middle of a transfer.
//
// Every chunk this covers is one fewer 35ms freeze, and a freeze is what makes
// the computer decide the drive is not there.
u32 ScsiImage::PreloadCache(u32 maxBytes, volatile u32* progressSectors, volatile u32* totalSectors)
{
	if (!cacheSlots || !attached || !maxBytes)
		return 0;

	// Leave a quarter of the cache hashed, so other images and anything past
	// the preloaded region still have somewhere to go.
	u32 chunks = maxBytes >> CACHE_CHUNK_SHIFT;
	u32 imageChunks = (sizeInSectors + SECTORS_PER_CHUNK - 1) / SECTORS_PER_CHUNK;
	u32 limit = numCacheSlots - (numCacheSlots >> 2);
	if (chunks > imageChunks)
		chunks = imageChunks;
	if (chunks > limit)
		chunks = limit;
	if (!chunks)
		return 0;

	// Claim the region before filling it, so FindSlot hands back slot N for
	// chunk N as each one lands.
	pinnedChunks = chunks;
	pinnedImage = imageId;

	if (totalSectors)
		*totalSectors = chunks * SECTORS_PER_CHUNK;

	// Chunk N is pinned to slot N and slot N's data is cachePool + N*4K, so the
	// whole region is contiguous in memory as well as in the file. Read it in
	// big blocks: chunk at a time meant thousands of separate seeks and reads,
	// which at this card's latency turned the mount into minutes.
	const u32 BLOCK = 1u << 20;
	u32 wanted = chunks * CACHE_CHUNK_SIZE;
	u32 done = 0;

	if (f_lseek(&file, 0) != FR_OK)
	{
		pinnedChunks = 0;
		if (totalSectors)
			*totalSectors = 0;
		return 0;
	}

	while (done < wanted)
	{
		u32 ask = wanted - done;
		if (ask > BLOCK)
			ask = BLOCK;

		UINT got = 0;
		if (f_read(&file, cachePool + done, ask, &got) != FR_OK)
			break;

		done += got;
		if (progressSectors)
			*progressSectors = done / SECTOR_SIZE;

		if (got < ask)
			break;			// end of file
	}

	// Anything the file did not cover reads as zeros, the same contract
	// FillChunk honours past EOF.
	if (done < wanted)
		memset(cachePool + done, 0, wanted - done);

	u32 loaded = done >> CACHE_CHUNK_SHIFT;

	for (u32 i = 0; i < chunks; ++i)
	{
		CacheSlot& slot = cacheSlots[i];
		slot.image = imageId;
		slot.chunkIndex = i;
		slot.dirtyMask = 0;
		// Only whole chunks that actually arrived are presented as valid. A
		// partial tail is left cold so the first read fills it the usual way
		// rather than serving a half read chunk as if it were real.
		if (i < loaded)
		{
			slot.valid = 1;
			slot.validMask = 0xff;
		}
		else
		{
			slot.valid = 0;
			slot.validMask = 0;
		}
	}

	if (totalSectors)
		*totalSectors = 0;

	DEBUG_LOG("SCSI: preloaded %u of %u chunks (%u KB pinned)\r\n",
		loaded, chunks, (chunks * CACHE_CHUNK_SIZE) >> 10);
	return loaded;
}

#if PICMD_ACCESS_FORENSICS
u8 ScsiImage::writeLogData[ScsiImage::WRITE_LOG_ENTRIES][ScsiImage::SECTOR_SIZE];
u32 ScsiImage::writeLogLba[ScsiImage::WRITE_LOG_ENTRIES];
u32 ScsiImage::writeLogSeq[ScsiImage::WRITE_LOG_ENTRIES];
u32 ScsiImage::writeLogCount = 0;
#endif

// snprintf returns what it would have written, not what it did. Handing that
// to f_write walks off the end of the buffer.
static UINT Clamp(int produced, size_t capacity)
{
	if (produced < 0)
		return 0;
	return (UINT)((size_t)produced < capacity ? (size_t)produced : capacity - 1);
}

#if PICMD_ACCESS_FORENSICS
// Every write as it was handed to us, payload and all. Dumped on eject next to
// the address log, so the two can be lined up by sequence number.
bool ScsiImage::DumpWriteLog(const char* prefix)
{
	if (writeLogCount == 0)
		return false;

	char filename[64];
	FILINFO fno;
	unsigned serial = 0;
	for (;; ++serial)
	{
		if (serial > 99)
			return false;
		snprintf(filename, sizeof(filename), "%s%02u.LOG", prefix, serial);
		if (f_stat(filename, &fno) != FR_OK)
			break;
	}

	FIL fp;
	if (f_open(&fp, filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return false;

	char line[160];
	UINT written;
	bool wrapped = writeLogCount > WRITE_LOG_ENTRIES;
	u32 held = wrapped ? WRITE_LOG_ENTRIES : writeLogCount;
	u32 first = wrapped ? writeLogCount - WRITE_LOG_ENTRIES : 0;

	int n = snprintf(line, sizeof(line), "# %u writes, %u held%s\r\n",
		writeLogCount, held, wrapped ? " (wrapped, oldest lost)" : "");
	f_write(&fp, line, Clamp(n, sizeof(line)), &written);

	for (u32 i = 0; i < held; ++i)
	{
		u32 slot = (first + i) % WRITE_LOG_ENTRIES;
		n = snprintf(line, sizeof(line), "W %u lba %u\r\n",
			writeLogSeq[slot], writeLogLba[slot]);
		if (f_write(&fp, line, Clamp(n, sizeof(line)), &written) != FR_OK)
			break;

		const u8* d = writeLogData[slot];
		for (u32 off = 0; off < SECTOR_SIZE; off += 32)
		{
			n = snprintf(line, sizeof(line), "%03x:", off);
			for (u32 j = 0; j < 32; ++j)
				n += snprintf(line + n, sizeof(line) - n, "%02x", d[off + j]);
			n += snprintf(line + n, sizeof(line) - n, "\r\n");
			if (f_write(&fp, line, Clamp(n, sizeof(line)), &written) != FR_OK)
			{
				f_close(&fp);
				return false;
			}
		}
	}

	f_close(&fp);
	return true;
}
#endif

#if PICMD_ACCESS_FORENSICS
void ScsiImage::LogAccess(u32 lba, u8 op, u8 result)
{
	accessLog[accessLogCount % ACCESS_LOG_ENTRIES].lba = lba;
	accessLog[accessLogCount % ACCESS_LOG_ENTRIES].op = op;
	accessLog[accessLogCount % ACCESS_LOG_ENTRIES].image = imageId;
	accessLog[accessLogCount % ACCESS_LOG_ENTRIES].result = result;
	accessLogCount++;

	if (op == 'W' && result == 0 && lba > maxLbaWritten)
		maxLbaWritten = lba;
}
#endif

#if PICMD_ACCESS_FORENSICS
// Write the log out as text. Only ever called from eject, when the bus is
// already done with us and a slow card costs nothing.
//
// The name gets a two digit serial because eject is also how you get back to
// the file browser: going back in to look at the result of a run and coming
// out again would otherwise overwrite the run you wanted to keep.
bool ScsiImage::DumpAccessLog(const char* prefix)
{
	if (accessLogCount == 0)
		return false;

	char filename[64];
	FILINFO fno;
	unsigned serial = 0;
	for (;; ++serial)
	{
		if (serial > 99)
			return false;		// 100 runs without emptying the card; give up
		snprintf(filename, sizeof(filename), "%s%02u.LOG", prefix, serial);
		if (f_stat(filename, &fno) != FR_OK)
			break;				// free name
	}

	FIL fp;
	if (f_open(&fp, filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return false;

	char line[128];
	UINT written;
	bool wrapped = accessLogCount > ACCESS_LOG_ENTRIES;
	u32 held = wrapped ? ACCESS_LOG_ENTRIES : accessLogCount;
	u32 first = wrapped ? accessLogCount - ACCESS_LOG_ENTRIES : 0;

	// Two lines, written separately: together they overrun a 128 byte buffer,
	// and snprintf reports the length it wanted rather than the length it
	// produced - so handing that straight to f_write read off the end of the
	// array and put rubbish in the header.
	int n = snprintf(line, sizeof(line),
		"# seq op lba image result   (op: R cached read, W write, U uncached read)\r\n");
	f_write(&fp, line, Clamp(n, sizeof(line)), &written);
	n = snprintf(line, sizeof(line), "# %u accesses, %u held%s, max lba written %u\r\n",
		accessLogCount, held, wrapped ? " (wrapped, oldest lost)" : "", maxLbaWritten);
	f_write(&fp, line, Clamp(n, sizeof(line)), &written);

	for (u32 i = 0; i < held; ++i)
	{
		const AccessLogEntry& e = accessLog[(first + i) % ACCESS_LOG_ENTRIES];
		n = snprintf(line, sizeof(line), "%u %c %u %u %u\r\n",
			first + i, (char)e.op, e.lba, (unsigned)e.image, (unsigned)e.result);
		UINT len = Clamp(n, sizeof(line));
		if (f_write(&fp, line, len, &written) != FR_OK || written != len)
		{
			f_close(&fp);
			return false;
		}
	}

	f_close(&fp);
	return true;
}
#endif

// Returns true only when there is nothing left owed anywhere. False means come
// back - the bus asked for the drive, a write failed, or the per-visit bound
// was reached - and needSync is still set on whatever is left.
ScsiImage::FlushResult ScsiImage::FlushIdle(bool (*abort)())
{
	for (u32 i = 0; i < numAttachedImages; ++i)
	{
		ScsiImage* img = attachedImages[i];
		if (img && img->attached && img->needSync)
		{
			// Stop the moment the bus wants us again rather than finishing the
			// backlog regardless. f_sync only once there is nothing left, or it
			// would be paying for metadata on every partial drain.
			FlushResult r = img->FlushAllDirty(abort, IDLE_FLUSH_CHUNKS);
			if (r != FLUSH_DONE)
				return r;
			f_sync(&img->file);
			img->needSync = false;
		}
	}
	return FLUSH_DONE;
}

u32 ScsiImage::DirtyChunkCount()
{
	u32 n = 0;
	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		if (cacheSlots[i].valid && cacheSlots[i].dirtyMask)
			n++;
	}
	return n;
}

ScsiImage* ScsiImage::ImageById(u8 id)
{
	for (u32 i = 0; i < numAttachedImages; ++i)
	{
		if (attachedImages[i] && attachedImages[i]->imageId == id)
			return attachedImages[i];
	}
	return 0;
}

void ScsiImage::NoteStall(u32 startMicros)
{
	u32 took = read32(ARM_SYSTIMER_CLO) - startMicros;
	if (took > worstStallMicros)
		worstStallMicros = took;
}

int ScsiImage::FillChunk(u32 chunkIndex, CacheSlot& slot)
{
	UINT bytesRead = 0;
	u64 offset = (u64)chunkIndex << CACHE_CHUNK_SHIFT;
	u32 t0 = read32(ARM_SYSTIMER_CLO);

	if (f_lseek(&file, offset) != FR_OK)
		return -1;
	if (f_read(&file, slot.data, CACHE_CHUNK_SIZE, &bytesRead) != FR_OK)
		return -1;
	NoteStall(t0);

	// Beyond EOF reads as zeros (matches VICE behaviour).
	if (bytesRead < CACHE_CHUNK_SIZE)
		memset(slot.data + bytesRead, 0, CACHE_CHUNK_SIZE - bytesRead);

	slot.valid = 1;
	slot.validMask = 0xff;
	slot.dirtyMask = 0;
	return 0;
}

int ScsiImage::ReadSector(u32 lba, u8* buffer)
{
	accessCounter++;
	if (!attached)
	{
		LogAccess(lba, 'R', 1);
		return -1;
	}

	if (lba >= sizeInSectors)
	{
		// Reads beyond the end of the image return zeros (matches VICE).
		LogAccess(lba, 'R', 1);
		memset(buffer, 0, SECTOR_SIZE);
		return 0;
	}

	LogAccess(lba, 'R', 0);

	u32 chunkIndex = lba / SECTORS_PER_CHUNK;
	u32 sectorInChunk = lba % SECTORS_PER_CHUNK;
	u8 bit = (u8)(1 << sectorInChunk);

	CacheSlot* slot = FindSlot(chunkIndex, true);
	if (!slot)
	{
		// No cache, or the set was full of chunks the card would not take.
		// Either way, read straight through.
		cacheReadMisses++;
		UINT bytesRead = 0;
		u32 t0 = read32(ARM_SYSTIMER_CLO);
		if (f_lseek(&file, (u64)lba << 9) != FR_OK)
			return -1;
		if (f_read(&file, buffer, SECTOR_SIZE, &bytesRead) != FR_OK)
			return -1;
		if (bytesRead < SECTOR_SIZE)
			memset(buffer + bytesRead, 0, SECTOR_SIZE - bytesRead);
		NoteStall(t0);
		return 0;
	}

	// Sampled before anything below can clear it. Note FillChunk only ever runs
	// with dirtyMask already zero, so it cannot race this.
	if (slot->dirtyMask)
		dirtyChunkReads++;

	if (slot->validMask & bit)
	{
		cacheReadHits++;
	}
	else
	{
		cacheReadMisses++;
		if (slot->dirtyMask == 0)
		{
			// Nothing to lose, so pull the whole chunk in and keep the
			// read-ahead that makes sequential reads cheap.
			if (FillChunk(chunkIndex, *slot) != 0)
			{
				slot->valid = 0;
				return -1;
			}
		}
		else
		{
			// Part of this chunk is written but not yet on the card, so a
			// whole-chunk read would clobber it. Fetch just this sector.
			dirtyChunkPartialFills++;
			UINT bytesRead = 0;
			u32 t0 = read32(ARM_SYSTIMER_CLO);
			if (f_lseek(&file, (u64)lba << 9) != FR_OK)
				return -1;
			if (f_read(&file, slot->data + sectorInChunk * SECTOR_SIZE,
					SECTOR_SIZE, &bytesRead) != FR_OK)
				return -1;
			if (bytesRead < SECTOR_SIZE)
				memset(slot->data + sectorInChunk * SECTOR_SIZE + bytesRead, 0,
					SECTOR_SIZE - bytesRead);
			NoteStall(t0);
			slot->validMask |= bit;
		}
	}

	memcpy(buffer, slot->data + sectorInChunk * SECTOR_SIZE, SECTOR_SIZE);
	return 0;
}

int ScsiImage::ReadSectorUncached(u32 lba, u8* buffer)
{
	accessCounter++;
	if (!attached)
	{
		LogAccess(lba, 'U', 1);
		return -1;
	}

	if (lba >= sizeInSectors)
	{
		LogAccess(lba, 'U', 1);
		memset(buffer, 0, SECTOR_SIZE);
		return 0;
	}

	LogAccess(lba, 'U', 0);

	// If the sector happens to be cached, take it from there - but never fill
	// the cache on its account. A slot being valid only says the chunk is
	// allocated: a write leaves the other seven sectors of it holding whatever
	// was there before, so the per sector bit has to be checked as well.
	u32 sectorInChunk = lba % SECTORS_PER_CHUNK;
	CacheSlot* slot = FindSlot(lba / SECTORS_PER_CHUNK, false);
	if (slot && slot->valid && (slot->validMask & (1 << sectorInChunk)))
	{
		memcpy(buffer, slot->data + sectorInChunk * SECTOR_SIZE, SECTOR_SIZE);
		return 0;
	}

	UINT bytesRead = 0;
	if (f_lseek(&file, (u64)lba << 9) != FR_OK)
		return -1;
	if (f_read(&file, buffer, SECTOR_SIZE, &bytesRead) != FR_OK || bytesRead != SECTOR_SIZE)
		return -1;
	return 0;
}

int ScsiImage::WriteSector(u32 lba, const u8* buffer)
{
	accessCounter++;
	if (!attached || readOnly)
	{
		LogAccess(lba, 'W', 1);
		return -1;
	}

	if (lba >= sizeInSectors)
	{
		LogAccess(lba, 'W', 1);
		return -1;
	}

	LogAccess(lba, 'W', 0);

#if PICMD_WRITE_FORENSICS
	// Keep the payload too. accessLogCount has just been bumped past this
	// access, so seq-1 is the entry in the address log that matches.
	{
		u32 slot = writeLogCount % WRITE_LOG_ENTRIES;
		memcpy(writeLogData[slot], buffer, SECTOR_SIZE);
		writeLogLba[slot] = lba;
		writeLogSeq[slot] = accessLogCount - 1;
		writeLogCount++;
	}
#endif

	u32 chunkIndex = lba / SECTORS_PER_CHUNK;
	u32 sectorInChunk = lba % SECTORS_PER_CHUNK;

	// Take the write into the cache and leave. This must not touch the card at
	// all: the computer polls us for status straight afterwards, and any SD
	// access freezes the drive for as long as the card takes.
	CacheSlot* slot = FindSlot(chunkIndex, true);
	if (slot)
	{
		if (!slot->valid)
		{
			slot->valid = 1;
			slot->validMask = 0;
			slot->dirtyMask = 0;
		}
#if PICMD_WRITE_FORENSICS
		// Before it is overwritten, ask what changed. See BitRotWrites in the
		// header: bytes that only ever gain bits 0 and 1 are the corruption,
		// and this is the one place the before and after are both to hand.
		if (slot->validMask & (1 << sectorInChunk))
		{
			const u8* was = slot->data + sectorInChunk * SECTOR_SIZE;
			u32 differing = 0;
			u32 gainedLowBits = 0;
			u8 gained = 0;
			for (u32 i = 0; i < SECTOR_SIZE; i++)
			{
				if (was[i] == buffer[i])
					continue;
				differing++;
				if ((u8)(buffer[i] & 0xfc) == was[i])
				{
					gainedLowBits++;
					gained |= (u8)(buffer[i] ^ was[i]);
				}
			}
			if (differing >= 3 && gainedLowBits == differing)
			{
				if (!bitRotWrites)
					bitRotFirstLba = lba;
				bitRotWrites++;
				bitRotBytes += gainedLowBits;
				bitRotMask |= gained;
			}
		}
#endif

		memcpy(slot->data + sectorInChunk * SECTOR_SIZE, buffer, SECTOR_SIZE);
		slot->validMask |= (u8)(1 << sectorInChunk);
		slot->dirtyMask |= (u8)(1 << sectorInChunk);
		needSync = true;
		return 0;
	}

	// No cache at all; nothing for it but to write straight through.
	UINT bytesWritten = 0;
	u32 t0 = read32(ARM_SYSTIMER_CLO);
	if (f_lseek(&file, (u64)lba << 9) != FR_OK)
		return -1;
	if (f_write(&file, buffer, SECTOR_SIZE, &bytesWritten) != FR_OK || bytesWritten != SECTOR_SIZE)
		return -1;
	NoteStall(t0);
	needSync = true;
	return 0;
}

// Push a chunk's dirty sectors out to the card, in as few writes as the
// runs of set bits allow.
int ScsiImage::FlushChunk(CacheSlot& slot)
{
	if (!slot.valid || !slot.dirtyMask)
		return 0;

	u32 s = 0;
	while (s < SECTORS_PER_CHUNK)
	{
		if (!(slot.dirtyMask & (1 << s))) { ++s; continue; }

		u32 run = 1;
		while (s + run < SECTORS_PER_CHUNK && (slot.dirtyMask & (1 << (s + run))))
			++run;

		UINT written = 0;
		u32 t0 = read32(ARM_SYSTIMER_CLO);
		u64 offset = ((u64)slot.chunkIndex << CACHE_CHUNK_SHIFT) + (u64)s * SECTOR_SIZE;
		if (f_lseek(&file, offset) != FR_OK)
			return -1;
		// A short write is not an error as far as FatFS is concerned - a full
		// card returns FR_OK having written less than asked. Treat it as one,
		// or the caller clears dirtyMask on sectors that never reached the SD.
		if (f_write(&file, slot.data + s * SECTOR_SIZE, run * SECTOR_SIZE, &written) != FR_OK
			|| written != run * SECTOR_SIZE)
			return -1;
		NoteStall(t0);
		s += run;
	}

	slot.dirtyMask = 0;
	return 0;
}

ScsiImage::FlushResult ScsiImage::FlushAllDirty(bool (*abort)(), u32 maxChunks)
{
	u32 done = 0;

	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		if (!(cacheSlots[i].valid && cacheSlots[i].dirtyMask && cacheSlots[i].image == imageId))
			continue;
		// Asked before committing to the write, not after: once FlushChunk is
		// under way the CPU is frozen for as long as the card takes.
		//
		// And asked BEFORE the bound below, not after. The other order looks
		// harmless and is not: with a bound of one chunk the function would
		// return at the second dirty slot without ever evaluating abort, so
		// the callback would become a thing that cannot fire - which is the
		// failure mode this project has documented three times.
		if (abort && abort())
			return FLUSH_STOPPED;

		// Stop after maxChunks and let the caller come back. The emulated CPU
		// is frozen for every card write in this loop, and unbounded that came
		// to 48 milliseconds in one go on a GEOS installation - measured, the
		// worst overrun in PICMD-LST37.LOG. A bound turns one long freeze into
		// several short ones with the loop running in between. maxChunks == 0
		// means no bound, for detach, where finishing matters more than
		// latency.
		if (maxChunks && done >= maxChunks)
			return FLUSH_MORE;
		// FlushChunk leaves dirtyMask set on purpose when the write did not
		// land, so the caller can come back for it. Dropping the return here
		// turned that into "we tried once and then forgot": Sync cleared
		// needSync on the strength of a true that meant nothing, and Detach
		// invalidated the slots underneath. A full or failing card lost the
		// user's sectors without a word.
		if (FlushChunk(cacheSlots[i]) != 0)
			return FLUSH_STOPPED;
		++done;
	}
	return FLUSH_DONE;
}

///////////////////////////////////////////////////////////////////////////////
// SCSI target state machine (from VICE scsi.c).
///////////////////////////////////////////////////////////////////////////////

static u32 scsi_getmaxsize(scsi_context_t* context)
{
	/* make sure the target and lun are valid */
	if (context->target >= MAXIDS || context->lun >= MAXLUNS)
	{
		return 0;
	}

	/* make sure there is a file associated with the target and lun */
	ScsiImage* image = context->file[(context->target << 3) | context->lun];
	if (image && image->IsAttached())
	{
		if (!context->max_imagesize)
		{
			/* if the max length setting is zero, return the image length */
			return image->SizeInSectors();
		}
		else
		{
			/* otherwise, return the setting itself */
			return context->max_imagesize;
		}
	}

	/* not a valid file, return 0 */
	return 0;
}

static s32 scsi_imagecheck(scsi_context_t* context)
{
	if (context->target >= MAXIDS || context->lun >= MAXLUNS)
	{
		return 1;
	}

	ScsiImage* image = context->file[(context->target << 3) | context->lun];
	if (!image || !image->IsAttached())
	{
		if (context->target == 0 && context->lun == 0 &&
			!(context->log & SCSI_LOG_NODISK0))
		{
			DEBUG_LOG("SCSI: no image attached to disk 0; expect unusual results and/or hangs\r\n");
			context->log |= SCSI_LOG_NODISK0;
		}
		return 2;
	}

	return 0;
}

// As scsi_image_read, but straight off the card. The whole disk scans step
// 128 sectors at a time; a 4K chunk fetch would read eight times the data,
// reuse none of it, and evict what the drive is actually working with.
s32 scsi_image_read_uncached(scsi_context_t* context)
{
	if (scsi_imagecheck(context))
	{
		return -1;
	}

	ScsiImage* image = context->file[(context->target << 3) | context->lun];

	if (image->ReadSectorUncached(context->address, context->data_buf) != 0)
	{
		return -4;
	}

	return 0;
}

s32 scsi_image_read(scsi_context_t* context)
{
	if (scsi_imagecheck(context))
	{
		return -1;
	}

	ScsiImage* image = context->file[(context->target << 3) | context->lun];

	if (image->ReadSector(context->address, context->data_buf) != 0)
	{
		DEBUG_LOG("SCSI: error reading disk %d at sector 0x%x\r\n", context->target, context->address);
		return -4;
	}

	/* if the user provides it, call a function to modify the data before
	   it goes back to the host */
	if (context->user_read)
	{
		context->user_read(context);
	}

	return 0;
}

s32 scsi_image_write(scsi_context_t* context)
{
	if (scsi_imagecheck(context))
	{
		return -1;
	}

	/* if the user provides it, call a function to modify the data before
	   it writes to disk */
	if (context->user_write)
	{
		context->user_write(context);
	}

	ScsiImage* image = context->file[(context->target << 3) | context->lun];

	if (image->WriteSector(context->address, context->data_buf) != 0)
	{
		DEBUG_LOG("SCSI: error writing disk %d at sector 0x%x\r\n", context->target, context->address);
		return -4;
	}

	return 0;
}

/* default format handler */
/* We don't actually format the disk, we just zero out the first sector */
static void scsi_format_sector0(scsi_context_t* context)
{
	s32 i;

	context->address = 0;

	for (i = 0; i < 512; i++)
	{
		context->data_buf[i] = 0;
	}

	scsi_image_write(context);
}

u8 scsi_get_bus(scsi_context_t* context)
{
	return context->databus;
}

int scsi_set_bus(scsi_context_t* context, u8 value)
{
	if (!context->io)
	{
		context->databus = value;
		return 0;
	}
	else
	{
		return 1;
	}
}

void scsi_process_noack(scsi_context_t* context)
{
	s32 i, t, n;

	/* handle reset condition */
	if (context->rst)
	{
		context->cmd_size = 256;
		context->target = 255;
		context->bsyo = 0;
		context->req = 0;
		context->io = 0;
		context->cd = 0;
		context->msg = 0;
		context->seq = 0;
		context->link = 0;
		context->state = SCSI_STATE_BUSFREE;
		return;
	}

	if (context->state != SCSI_STATE_BUSFREE)
	{
		return;
	}

	/* handle SELECTION phase here */
	if (context->sel && !context->bsyo)
	{
		/* obtain target */
		context->target = 0;
		/* remove initiator from list */
		t = (context->databus ^ 0xff) & 0x7f;
		n = 0;
		i = 0;
		while (t)
		{
			if (t & 1)
			{
				context->target = i;
				n++;
			}
			t = t >> 1;
			i++;
		}
		if (n == 1 && context->target < 7)
		{
			context->bsyo = 1;
			context->req = 0;
			context->seq = 0;
		}
		else
		{
			/* Not asking for single ID or < 7, don't respond */
			context->target = 255;
			context->cmd_size = 256;
		}
		return;
	}
	else if (context->sel && context->bsyo)
	{
		/* do nothing, we are asserting BSY waiting for SEL to drop */
	}
	else if (!context->sel && context->bsyo &&
		context->state == SCSI_STATE_BUSFREE)
	{
		/* initiator has dropped SEL, we switch to command mode, keep
			BSY set */
		context->state = SCSI_STATE_COMMAND;
		context->cmd_size = 256;
		context->req = 1;
	}
}

void scsi_process_ack(scsi_context_t* context)
{
	u8 data = context->databus ^ 0xff;
	u8 byte;
	u32 j;
	s32 i;
	static const unsigned char VENDORID[9] = "PI-CMD  "; /* 8 chars */
	static const unsigned char PRODID[17] = "SCSI Image File "; /* 16 chars */
	static const unsigned char REVISION[5] = "1.0 "; /* 4 chars */

	if (context->state == SCSI_STATE_BUSFREE)
	{
		return;
	}

	if (context->state == SCSI_STATE_STATUS)
	{
		context->cd = 1;
		if (context->link)
		{
			context->seq = 0;
			context->state = SCSI_STATE_COMMAND;
			context->io = 0;
		}
		else
		{
			if (context->msg_after_status)
			{
				/* LTK expects drive to go to MESSAGEIN after STATUS instead
					of BUSFREE */
				context->state = SCSI_STATE_MESSAGEIN;
				context->io = 1;
				context->msg = 1;
				context->databus = 0 ^ 0xff;  /* just incase we set it 0 */
			}
			else
			{
				context->state = SCSI_STATE_BUSFREE;
				context->req = 0;
				context->io = 0;
				context->msg = 0;
				context->cd = 0;
			}
			context->bsyo = 0;
			goto out;
		}
	}

	if (context->state == SCSI_STATE_MESSAGEIN)
	{
		context->state = SCSI_STATE_BUSFREE;
		context->req = 0;
		context->bsyo = 0;
		context->io = 0;
		context->msg = 0;
		context->cd = 0;
		goto out;
	}

	if (context->state == SCSI_STATE_DATAOUT)
	{
		do
		{
			context->io = 0;
			context->cd = 0;
			// seq is the index, and it is bumped past the end deliberately to
			// mark a full buffer - and REASSIGN BLOCKS below parks it there
			// outright - so it has to be checked before it is used, not after.
			if (context->seq < sizeof(context->data_buf))
				context->data_buf[context->seq] = data;
			context->seq++;
			if (context->seq >= context->data_max)
			{
				/* write if WRITE command */
				if (context->command == SCSI_COMMAND_WRITE_6 ||
					context->command == SCSI_COMMAND_WRITE_10 ||
					context->command == SCSI_COMMAND_WRITE_VERIFY)
				{
					if (scsi_image_write(context))
					{
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					context->seq = 0;
					context->blocks--;
					context->address++;
					/* count down the number of blocks received, keep going
						if necessary */
					if (!context->blocks)
					{
						context->status = SCSI_STATUS_GOOD;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					if (context->address >= scsi_getmaxsize(context))
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->asc = SCSI_SASC_LOGICALBLOCKADDRESSOUTOFRANGE;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
						break;
					}
				}
				else if (context->command == SCSI_COMMAND_FORMAT_UNIT)
				{
					for (i = 0; i < 4; i++)
					{
						if (context->data_buf[i])
						{
							break;
						}
					}
					/* can read defect list header, but it has to be all
						zeros */
					if (i != 4)
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->asc = SCSI_SASC_INVALIDFIELDINPARAMETERLIST;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
					}
					else
					{
						context->status = SCSI_STATUS_GOOD;
						context->state = SCSI_STATE_STATUS;
						context->user_format(context);
					}
				}
				else if (context->command == SCSI_COMMAND_REASSIGN_BLOCKS)
				{
					/* accept defect list, just don't do anything about it */
					if (context->seq == 4)
					{
						context->seq = ((context->data_buf[0] << 24) |
							(context->data_buf[1] << 16) |
							(context->data_buf[2] << 8) |
							(context->data_buf[3])) + 4;
						if (context->seq >= sizeof(context->data_buf))
						{
							context->seq = sizeof(context->data_buf) - 1;
						}
					}
					else
					{
						context->status = SCSI_STATUS_GOOD;
						context->state = SCSI_STATE_STATUS;
					}
				}
				else
				{
					/* otherwise, finish up */
					context->status = SCSI_STATUS_GOOD;
					context->state = SCSI_STATE_STATUS;
				}
			}
		} while (0);
	}

	if (context->state == SCSI_STATE_COMMAND)
	{
		do
		{
			context->io = 0;
			context->cd = 1;
			context->cmd_buf[context->seq] = data;

			if (context->seq == 0)
			{
				context->command = context->cmd_buf[0];
				switch (context->command)
				{
				case SCSI_COMMAND_TEST_UNIT_READY:
				case SCSI_COMMAND_REZERO_UNIT:
				case SCSI_COMMAND_REQUEST_SENSE:
				case SCSI_COMMAND_FORMAT_UNIT:
				case SCSI_COMMAND_REASSIGN_BLOCKS:
				case SCSI_COMMAND_READ_6:
				case SCSI_COMMAND_WRITE_6:
				case SCSI_COMMAND_INQUIRY:
				case SCSI_COMMAND_MODE_SENSE:
				case SCSI_COMMAND_START_STOP:
				case SCSI_COMMAND_SEND_DIAGNOSTIC:
					context->cmd_size = 6;
					break;
				case SCSI_COMMAND_READ_CAPACITY:
				case SCSI_COMMAND_READ_10:
				case SCSI_COMMAND_WRITE_10:
				case SCSI_COMMAND_MODE_SENSE_10:
				case SCSI_COMMAND_WRITE_VERIFY:
				case SCSI_COMMAND_VERIFY:
					context->cmd_size = 10;
					break;
				default:
					DEBUG_LOG("SCSI: Unknown Command=%0x\r\n", context->cmd_buf[0]);
					context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
					context->status = SCSI_STATUS_CHECKCONDITION;
					context->state = SCSI_STATE_STATUS;
					break;
				}
			}
			context->seq++;
			if (context->seq >= context->cmd_size)
			{
				switch (context->command)
				{
				case SCSI_COMMAND_TEST_UNIT_READY:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[5] & 1;
					if (scsi_imagecheck(context))
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
					}
					else
					{
						context->status = SCSI_STATUS_GOOD;
						context->state = SCSI_STATE_STATUS;
					}
					break;
				case SCSI_COMMAND_REQUEST_SENSE:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[5] & 1;
					context->data_max = context->cmd_buf[4];
					if (!context->data_max)
					{
						context->data_max = 4;
					}
					if (context->data_max > 18)
					{
						context->data_max = 18;
					}
					for (i = 0; i < 512; i++)
					{
						context->data_buf[i] = 0;
					}
					context->data_buf[0] = 0x80 | 0x70;
					context->data_buf[1] = 0x00;
					context->data_buf[2] = context->sensekey;
					context->data_buf[12] = context->asc;
					context->status = SCSI_STATUS_GOOD;
					context->state = SCSI_STATE_STATUS;
					break;
				case SCSI_COMMAND_REASSIGN_BLOCKS:
					context->state = SCSI_STATE_DATAOUT;
					context->data_max = 4;
					context->seq = 0;
					break;
				case SCSI_COMMAND_READ_6:
				case SCSI_COMMAND_READ_10:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					if (context->command == SCSI_COMMAND_READ_6)
					{
						context->link = context->cmd_buf[5] & 1;
						context->address = ((context->cmd_buf[1] & 0x1f) << 16) |
							(context->cmd_buf[2] << 8) | (context->cmd_buf[3]);
						context->blocks = context->cmd_buf[4];
						/* if blocks=0, it really means 256 */
						if (!context->blocks)
						{
							context->blocks = 256;
						}
					}
					else
					{
						context->link = context->cmd_buf[9] & 1;
						context->address = (context->cmd_buf[2] << 24) |
							(context->cmd_buf[3] << 16) |
							(context->cmd_buf[4] << 8) | (context->cmd_buf[5]);
						context->blocks = (context->cmd_buf[7] << 8) |
							(context->cmd_buf[8]);
						/* if blocks=0, it really means 0 */
						if (!context->blocks)
						{
							context->status = SCSI_STATUS_GOOD;
							context->state = SCSI_STATE_STATUS;
							break;
						}
					}
					context->data_max = 512;
					if (context->address >= scsi_getmaxsize(context))
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->asc = SCSI_SASC_LOGICALBLOCKADDRESSOUTOFRANGE;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					if (!scsi_image_read(context))
					{
						context->state = SCSI_STATE_DATAIN;
						context->seq = 0;
					}
					else
					{
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
					}
					break;
				case SCSI_COMMAND_WRITE_6:
				case SCSI_COMMAND_WRITE_10:
				case SCSI_COMMAND_WRITE_VERIFY:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					if (context->command == SCSI_COMMAND_WRITE_6)
					{
						context->link = context->cmd_buf[5] & 1;
						context->address = ((context->cmd_buf[1] & 0x1f) << 16) |
							(context->cmd_buf[2] << 8) | (context->cmd_buf[3]);
						context->blocks = context->cmd_buf[4];
						/* if blocks=0, it really means 256 */
						if (!context->blocks)
						{
							context->blocks = 256;
						}
					}
					else
					{
						context->link = context->cmd_buf[9] & 1;
						context->address = (context->cmd_buf[2] << 24) |
							(context->cmd_buf[3] << 16) |
							(context->cmd_buf[4] << 8) | (context->cmd_buf[5]);
						context->blocks = (context->cmd_buf[7] << 8) |
							(context->cmd_buf[8]);
						/* if blocks=0, it really means 0 */
						if (!context->blocks)
						{
							context->status = SCSI_STATUS_GOOD;
							context->state = SCSI_STATE_STATUS;
							break;
						}
					}
					context->data_max = 512;
					if (context->address >= scsi_getmaxsize(context))
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->asc = SCSI_SASC_LOGICALBLOCKADDRESSOUTOFRANGE;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					if (!scsi_imagecheck(context))
					{
						context->state = SCSI_STATE_DATAOUT;
						context->seq = 0;
					}
					else
					{
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
					}
					break;
				case SCSI_COMMAND_INQUIRY:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[5] & 1;
					context->data_max = context->cmd_buf[4]; /* usually 96 */
					for (i = 0; i < 512; i++)
					{
						context->data_buf[i] = 0;
					}
					if (scsi_imagecheck(context))
					{
						context->data_buf[0] = 0x60;
					}
					else
					{
						context->data_buf[0] = 0x00;
					}
					context->data_buf[1] = 0x00; /* not removable */
					context->data_buf[2] = 0x01; /* SCSI 1 */
					context->data_buf[3] = 0x02;
					context->data_buf[4] = 92; /* no additional data */
					context->data_buf[5] = 0x00; /* res */
					context->data_buf[6] = 0x00; /* res */
					context->data_buf[7] = 0x00;
					for (i = 0; i < 8; i++)
					{
						context->data_buf[i + 8] = VENDORID[i];
					}
					for (i = 0; i < 16; i++)
					{
						context->data_buf[i + 16] = PRODID[i];
					}
					for (i = 0; i < 4; i++)
					{
						context->data_buf[i + 32] = REVISION[i];
					}
					context->state = SCSI_STATE_DATAIN;
					context->seq = 0;
					break;
				case SCSI_COMMAND_READ_CAPACITY:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[9] & 1;
					context->data_max = 8;
					j = scsi_getmaxsize(context);
					if (j == 0)
					{
						i = 0;
					}
					else
					{
						/* Limit size returned as some tools can't handle large disks */
						if (j > context->limit_imagesize)
						{
							j = context->limit_imagesize;
						}
						i = 512;
						/* SCSI spec says return last accessible block number */
						j--;
					}
					context->data_buf[0] = (j >> 24) & 255;
					context->data_buf[1] = (j >> 16) & 255;
					context->data_buf[2] = (j >> 8) & 255;
					context->data_buf[3] = j & 255;
					context->data_buf[4] = (i >> 24) & 255;
					context->data_buf[5] = (i >> 16) & 255;
					context->data_buf[6] = (i >> 8) & 255;
					context->data_buf[7] = i & 255;
					context->state = SCSI_STATE_DATAIN;
					context->seq = 0;
					break;
				case SCSI_COMMAND_MODE_SENSE:
				case SCSI_COMMAND_MODE_SENSE_10:
					context->state = SCSI_STATE_DATAIN;
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->seq = 0;
					if (context->command == SCSI_COMMAND_MODE_SENSE)
					{
						context->data_max = context->cmd_buf[4];
						context->link = context->cmd_buf[5] & 1;
					}
					else
					{
						context->data_max = (context->cmd_buf[7] << 8) |
							(context->cmd_buf[8]);
						if (context->data_max > 255)
						{
							context->data_max = 255;
						}
						context->link = context->cmd_buf[9] & 1;
					}
					for (i = 0; i < 512; i++)
					{
						context->data_buf[i] = 0;
					}
					/* CMD drive info tool looks at page 20h, so lets put that one in */
					switch (context->cmd_buf[2])
					{
						case 0x00 | 0x20:
							context->data_buf[3] = 1;
							for (i = 0; i < (s32)context->data_max - (3 + 1 + 3) &&
								REVISION[i]; i++)
							{
								context->data_buf[3 + 1 + 3 + i] = REVISION[i];
							}
							context->data_buf[3 + 1 + 2] = i;
							break;
						default:
							context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
							context->asc = SCSI_SASC_INVALIDFIELDINCDB;
							context->status = SCSI_STATUS_CHECKCONDITION;
							context->state = SCSI_STATE_STATUS;
							break;
					}
					break;
				case SCSI_COMMAND_FORMAT_UNIT:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[5] & 1;
					/* check if fmtdata is set, move on to dataout */
					if ((context->cmd_buf[1] & 0x17) == 0x10)
					{
						context->state = SCSI_STATE_DATAOUT;
						context->seq = 0;
						context->data_max = 4;
					/* cmplist can be anything, but list format must be 0 */
					}
					else if ((context->cmd_buf[1] & 0x17) == 0x00)
					{
						context->status = SCSI_STATUS_GOOD;
						context->state = SCSI_STATE_STATUS;
						context->user_format(context);
					}
					else
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->asc = SCSI_SASC_INVALIDFIELDINPARAMETERLIST;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
					}
					break;
				case SCSI_COMMAND_REZERO_UNIT:
				case SCSI_COMMAND_START_STOP:
				case SCSI_COMMAND_SEND_DIAGNOSTIC:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[5] & 1;
					context->status = SCSI_STATUS_GOOD;
					context->state = SCSI_STATE_STATUS;
					break;
				case SCSI_COMMAND_VERIFY:
					context->lun = (context->cmd_buf[1] >> 5) & 7;
					context->link = context->cmd_buf[9] & 1;
					context->status = SCSI_STATUS_GOOD;
					context->state = SCSI_STATE_STATUS;
					break;
				}
			}
		} while (0);
	}

	if (context->state == SCSI_STATE_DATAIN)
	{
		do
		{
			if (context->seq >= context->data_max)
			{
				/* reload more data if READ command */
				if (context->command == SCSI_COMMAND_READ_6 ||
					context->command == SCSI_COMMAND_READ_10)
				{
					context->seq = 0;
					context->blocks--;
					context->address++;
					/* count down the number of blocks sent, keep going if
						necessary */
					if (!context->blocks)
					{
						context->status = SCSI_STATUS_GOOD;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					/* if we are out of range, report an error */
					if (context->address >= scsi_getmaxsize(context))
					{
						context->sensekey = SCSI_SENSEKEY_ILLEGALREQUEST;
						context->asc = SCSI_SASC_LOGICALBLOCKADDRESSOUTOFRANGE;
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					/* read it, report error if a problem */
					if (scsi_image_read(context))
					{
						context->status = SCSI_STATUS_CHECKCONDITION;
						context->state = SCSI_STATE_STATUS;
						break;
					}
					/* all is good, keep state the same */
				}
				else
				{
					/* otherwise, finish up */
					context->status = SCSI_STATUS_GOOD;
					context->state = SCSI_STATE_STATUS;
					break;
				}
				/* never gets here */
			}
			byte = context->data_buf[context->seq];
			context->seq++;
			context->databus = byte ^ 0xff;
			context->io = 1;
			context->cd = 0;
		} while (0);
	}

	if (context->state == SCSI_STATE_STATUS)
	{
		byte = (context->status << 1) | 0;
		context->io = 1;
		context->databus = byte ^ 0xff;
		context->cd = 1;
	}

	if (context->state == SCSI_STATE_DATAOUT)
	{
		context->cd = 0;
	}

out:
	// Nothing is flushed here on purpose. This used to sweep every image on the
	// way back to BUSFREE, which is precisely when the computer polls us for
	// status - see the note on ScsiImage::Sync. The sweep had in fact been dead
	// for a while, because Sync() without force returns immediately; it is gone
	// now so the next reader is not misled into thinking writes land here.
	return;
}

void scsi_reset(scsi_context_t* context)
{
	context->max_imagesize = 0;			/* 0 = report the actual image size */
	context->limit_imagesize = 512 * 1024 * 1024 / 512;
	context->rst = 1;
	scsi_process_noack(context);
	context->rst = 0;
	context->msg_after_status = 0;
	context->user_format = scsi_format_sector0;
	context->user_read = 0;
	context->user_write = 0;
	context->log = 0;
}
