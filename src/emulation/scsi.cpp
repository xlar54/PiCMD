// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// SCSI disk (target) emulation.
// Ported from VICE's scsi.c written by Roberto Muscedere, adapted to
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

#include <string.h>
#include <stdlib.h>
#include "scsi.h"
#include "debug.h"
#include "rpihardware.h"

#define MAXIDS 7
#define MAXLUNS 8

///////////////////////////////////////////////////////////////////////////////
// ScsiImage - a FatFS backed hard disk image with a write-through cache.
///////////////////////////////////////////////////////////////////////////////

u8* ScsiImage::cachePool = 0;
ScsiImage::CacheSlot* ScsiImage::cacheSlots = 0;
u32 ScsiImage::numCacheSlots = 0;
u32 ScsiImage::nextImageId = 0;

void ScsiImage::InitCache(u32 sizeInBytes)
{
	if (cachePool)
		return;

	numCacheSlots = sizeInBytes >> CACHE_CHUNK_SHIFT;
	if (numCacheSlots < 16)
		numCacheSlots = 16;

	cachePool = (u8*)malloc(numCacheSlots * CACHE_CHUNK_SIZE);
	cacheSlots = (CacheSlot*)malloc(numCacheSlots * sizeof(CacheSlot));

	if (!cachePool || !cacheSlots)
	{
		DEBUG_LOG("SCSI: failed to allocate %u byte cache\r\n", numCacheSlots * CACHE_CHUNK_SIZE);
		free(cachePool);
		free(cacheSlots);
		cachePool = 0;
		cacheSlots = 0;
		numCacheSlots = 0;
		return;
	}

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
	imageId = nextImageId++;
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
		// One retry, because the usual cause of a first failure is a transient
		// card problem. After that the file has to be closed regardless - there
		// is nowhere else for the data to go - so all that is left is to say so
		// loudly rather than invalidate the slots as if nothing happened.
		//
		// The report only counts; it must not flush again, or the diagnostic
		// becomes a third write attempt and can report zero after succeeding.
		if (Sync(true) < 0 && Sync(true) < 0)
			DEBUG_LOG("SCSI: '%s' detached with unwritten data - DATA LOST\r\n", name);
		else if (HasDirtyChunks())
			DEBUG_LOG("SCSI: '%s' detached with chunks still dirty - DATA LOST\r\n", name);

		// f_close writes the directory entry, so it is the last chance for
		// anything to go wrong and worth reporting too.
		if (f_close(&file) != FR_OK)
			DEBUG_LOG("SCSI: closing '%s' failed - the file may be truncated\r\n", name);
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

		for (u32 i = 0; i < numCacheSlots; ++i)
		{
			if (cacheSlots[i].valid && cacheSlots[i].image == imageId)
				cacheSlots[i].valid = 0;
		}
	}
}

int ScsiImage::Sync(bool force, u32 deadline)
{
	if (!attached || !needSync)
		return 0;

	// Only ever flush on demand. This used to run on a timer from the SCSI
	// state machine returning to BUSFREE - which is between commands, exactly
	// when the computer is about to poll for status. A single SD access has
	// been measured at 35ms on this hardware, far longer than the bus will
	// wait, so the drive vanished mid-transfer. Flushing now happens only when
	// the bus has gone quiet (FlushAll from the idle check), on reset, or on
	// detach.
	if (!force)
		return 0;

	int failed = FlushAllDirty(deadline);

	// Partial means "there is still dirty data", not "the budget was reached".
	// Those differ when the budget lands exactly on the last chunk, and
	// treating that as partial would skip f_sync and leave needSync set on a
	// cache that is in fact clean.
	bool partial = HasDirtyChunks();

	// f_sync pushes FatFS's own metadata (the directory entry and FAT chain).
	// Losing that loses the file, not just the sectors, so it counts too. Skip
	// it on a partial pass: there is more to write, and this is the expensive
	// part of a budgeted flush.
	if (!partial && f_sync(&file) != FR_OK)
		++failed;

	if (failed)
	{
		// Leave needSync set: the dirty chunks are still dirty and the next
		// flush must try them again.
		writeErrorPending = true;
		++writeErrors;
		DEBUG_LOG("SCSI: %d chunk(s) failed to write on '%s' - data kept for retry\r\n", failed, name);
		return -1;
	}

	// Only clean if everything actually went out.
	if (!partial)
		needSync = false;
	return partial ? 1 : 0;
}

// Write a slot's dirty sectors back so it can be reused. Returns false if the
// data could not be written and the slot must therefore be left alone.
bool ScsiImage::EvictSlot(CacheSlot* victim)
{
	ScsiImage* owner = ImageById(victim->image);
	if (!owner)
	{
		// The image was detached without this chunk being written. Nothing can
		// write it now - the FIL is closed - so it is already lost; say so
		// rather than quietly reusing the slot.
		DEBUG_LOG("SCSI: dirty chunk for detached image %u dropped\r\n", victim->image);
		++writeErrors;
		victim->dirtyMask = 0;
		return true;
	}

	if (owner->FlushChunk(*victim) != 0)
	{
		owner->writeErrorPending = true;
		++writeErrors;
		return false;
	}

	return true;
}

ScsiImage::CacheSlot* ScsiImage::FindSlot(u32 chunkIndex, bool allocate)
{
	if (!cacheSlots)
		return 0;

	// 2-way set associative on a multiplicative hash.
	u32 hash = (chunkIndex * 2654435761u + imageId * 40503u) % numCacheSlots;
	u32 hash2 = (hash + 1) % numCacheSlots;

	CacheSlot* slot = &cacheSlots[hash];
	if (slot->valid && slot->image == imageId && slot->chunkIndex == chunkIndex)
		return slot;
	CacheSlot* slot2 = &cacheSlots[hash2];
	if (slot2->valid && slot2->image == imageId && slot2->chunkIndex == chunkIndex)
		return slot2;

	if (!allocate)
		return 0;

	// Invalid first, then clean, then dirty. Evicting a dirty slot means going
	// to the card from inside a SCSI command - the thing this whole design
	// exists to avoid - so a clean slot is worth taking even when the other
	// way would be the more natural victim.
	CacheSlot* victim;
	if (!slot->valid)			victim = slot;
	else if (!slot2->valid)		victim = slot2;
	else if (!slot->dirtyMask)	victim = slot;
	else if (!slot2->dirtyMask)	victim = slot2;
	else						victim = slot;		// both dirty; one has to go

	// Never drop writes on the floor. The chunk being evicted may belong to
	// another image, so flush it through whoever owns it.
	//
	// If it cannot be written - card full, card gone, or the owning image has
	// been detached - the data must not be evicted, because the cache is the
	// only copy. Try the other way of the set, and if that is unwritable too,
	// refuse to allocate. WriteSector then falls back to writing straight
	// through, which is slow but reports its own failure honestly.
	if (victim->valid && victim->dirtyMask && !EvictSlot(victim))
	{
		CacheSlot* other = (victim == slot) ? slot2 : slot;
		if (other == victim || (other->valid && other->dirtyMask && !EvictSlot(other)))
			return 0;
		victim = other;
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
u32 ScsiImage::writeErrors = 0;

int ScsiImage::FlushSome(u32 maxMicros)
{
	int failed = 0;

	// One deadline for the whole call, so the bound holds however many images
	// are mounted rather than being per image.
	u32 deadline = maxMicros ? (read32(ARM_SYSTIMER_CLO) + maxMicros) : 0;

	for (u32 i = 0; i < numAttachedImages; ++i)
	{
		if (deadline && (s32)(read32(ARM_SYSTIMER_CLO) - deadline) >= 0)
			break;

		ScsiImage* img = attachedImages[i];
		if (img && img->attached && img->needSync)
		{
			if (img->Sync(true, deadline) < 0)
				++failed;
		}
	}

	return failed;
}

int ScsiImage::FlushAll()
{
	return FlushSome(0);
}

ScsiImage* ScsiImage::ImageById(u32 id)
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
		return -1;

	if (lba >= sizeInSectors)
	{
		// Reads beyond the end of the image return zeros (matches VICE).
		memset(buffer, 0, SECTOR_SIZE);
		return 0;
	}

	u32 chunkIndex = lba / SECTORS_PER_CHUNK;
	u32 sectorInChunk = lba % SECTORS_PER_CHUNK;
	u8 bit = (u8)(1 << sectorInChunk);

	CacheSlot* slot = FindSlot(chunkIndex, true);
	if (!slot)
	{
		// No cache; read straight through.
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

	if (!(slot->validMask & bit))
	{
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
		return -1;

	if (lba >= sizeInSectors)
	{
		memset(buffer, 0, SECTOR_SIZE);
		return 0;
	}

	// If the sector happens to be cached, take it from there - but never fill
	// the cache on its account.
	//
	// validMask has to be checked, not just valid: a slot created by writing
	// one sector holds nothing but uninitialised RAM for the other seven, and
	// handing that back as disk contents is how the base LBA scan ends up
	// finding a CMD signature that was never on the card.
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
		return -1;


	if (lba >= sizeInSectors)
		return -1;

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
//
// deadline (0 for none) is checked between runs, so a budgeted flush can stop
// part way through a chunk. That is safe because only the bits that actually
// reached the card are cleared - what is left simply stays dirty. Eviction
// passes no deadline: it needs the whole chunk out before it can reuse the
// slot.
int ScsiImage::FlushChunk(CacheSlot& slot, u32 deadline)
{
	if (!slot.valid || !slot.dirtyMask)
		return 0;

	u32 s = 0;
	while (s < SECTORS_PER_CHUNK)
	{
		if (!(slot.dirtyMask & (1 << s))) { ++s; continue; }

		if (deadline && (s32)(read32(ARM_SYSTIMER_CLO) - deadline) >= 0)
			return 0;

		u32 run = 1;
		while (s + run < SECTORS_PER_CHUNK && (slot.dirtyMask & (1 << (s + run))))
			++run;

		UINT written = 0;
		u32 t0 = read32(ARM_SYSTIMER_CLO);
		u64 offset = ((u64)slot.chunkIndex << CACHE_CHUNK_SHIFT) + (u64)s * SECTOR_SIZE;
		if (f_lseek(&file, offset) != FR_OK)
			return -1;
		// A short write counts as a failure. FatFS reports one on a full card
		// without setting an error, and taking it for success is how a full
		// card used to turn into silent corruption: the run below would clear
		// the dirty bits and the only copy of the data went away.
		if (f_write(&file, slot.data + s * SECTOR_SIZE, run * SECTOR_SIZE, &written) != FR_OK ||
			written != run * SECTOR_SIZE)
			return -1;
		NoteStall(t0);

		// Clear only what actually reached the card, so a failure part way
		// through a chunk leaves the rest dirty for the next attempt.
		slot.dirtyMask &= (u8)~(((1 << run) - 1) << s);
		s += run;
	}

	return 0;
}

int ScsiImage::FlushAllDirty(u32 deadline)
{
	int failed = 0;

	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		// Budget in real time, not in chunks. A chunk is up to four separate
		// writes when its dirty sectors are not contiguous, so counting chunks
		// bounds the work by a factor of four at best. An absolute deadline
		// also shares itself across images for free.
		//
		// Signed difference so it stays correct across the timer's ~71.6
		// minute wrap.
		if (deadline && (s32)(read32(ARM_SYSTIMER_CLO) - deadline) >= 0)
			break;

		if (cacheSlots[i].valid && cacheSlots[i].dirtyMask && cacheSlots[i].image == imageId)
		{
			if (FlushChunk(cacheSlots[i], deadline) != 0)
				++failed;
		}
	}

	return failed;
}

bool ScsiImage::HasDirtyChunks() const
{
	for (u32 i = 0; i < numCacheSlots; ++i)
	{
		if (cacheSlots[i].valid && cacheSlots[i].dirtyMask && cacheSlots[i].image == imageId)
			return true;
	}
	return false;
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

// A background flush failed since the last command was processed. The host was
// told that write succeeded and cannot be told otherwise, so it gets reported
// against a later command instead.
//
// Called from TEST UNIT READY, READ and WRITE - not from every command. Those
// three are what a host actually does after writing (and TEST UNIT READY is
// what it polls with), so the failure surfaces promptly in practice. The
// commands that do not check - READ CAPACITY, MODE SENSE and friends - are
// ones a host issues about the device rather than about its data, and adding
// the check there would report a data error in answer to a question that was
// not about data.
static s32 scsi_imagecheck(scsi_context_t* context);

static bool scsi_take_deferred_write_error(scsi_context_t* context)
{
	if (scsi_imagecheck(context))
		return false;

	ScsiImage* image = context->file[(context->target << 3) | context->lun];
	return image && image->TakeWriteError();
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
/* Returns 0 on success, non-zero if the write did not happen. */
static s32 scsi_format_sector0(scsi_context_t* context)
{
	s32 i;

	context->address = 0;

	for (i = 0; i < 512; i++)
	{
		context->data_buf[i] = 0;
	}

	// The result matters: on read-only media or a failing card this write does
	// not happen, and reporting FORMAT UNIT as successful when the disk is
	// untouched is worse than reporting the failure.
	return scsi_image_write(context);
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
			context->data_buf[context->seq] = data;
			context->seq++;
			if (context->seq >= context->data_max)
			{
				/* write if WRITE command */
				if (context->command == SCSI_COMMAND_WRITE_6 ||
					context->command == SCSI_COMMAND_WRITE_10 ||
					context->command == SCSI_COMMAND_WRITE_VERIFY)
				{
					// A deferred flush failed since the last host command. The
					// computer was told that earlier write succeeded and cannot
					// be told otherwise now, so report it against this one and
					// let HDOS surface it. The data is still dirty in the cache
					// and will be retried.
					//
					// This is checked here rather than inside WriteSector so
					// that the drive's own internal writes - the base LBA scan,
					// the format handler - cannot consume the latched error
					// before the host ever sees it.
					ScsiImage* img = scsi_imagecheck(context) ? 0 :
						context->file[(context->target << 3) | context->lun];

					if (scsi_image_write(context) || (img && img->TakeWriteError()))
					{
						// Say why, so REQUEST SENSE returns something
						// meaningful instead of whatever was left over from
						// the last command.
						context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
						context->asc = SCSI_SASC_WRITEFAULT;
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
						// Run the handler first and report what it did. This used to set
						// GOOD before calling it and ignore the result, so a format that
						// never touched the disk - read only media, a failing card -
						// still looked like a success.
						context->state = SCSI_STATE_STATUS;
						if (context->user_format(context))
						{
							context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
							context->asc = SCSI_SASC_WRITEFAULT;
							context->status = SCSI_STATUS_CHECKCONDITION;
						}
						else
						{
							context->status = SCSI_STATUS_GOOD;
						}
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
						if (context->seq > 512)
						{
							context->seq = 512;
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
					context->state = SCSI_STATE_STATUS;
					if (scsi_imagecheck(context))
					{
						// Nothing attached is NOT READY / medium not present,
						// not ILLEGAL REQUEST - and it used to set no ASC at
						// all, so the sense described the previous command.
						context->sensekey = SCSI_SENSEKEY_NOTREADY;
						context->asc = SCSI_SASC_MEDIUMNOTPRESENT;
						context->status = SCSI_STATUS_CHECKCONDITION;
					}
					else if (scsi_take_deferred_write_error(context))
					{
						// This is the command a host polls with, so it is the
						// one most likely to be the first thing issued after
						// the writing has finished.
						context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
						context->asc = SCSI_SASC_WRITEFAULT;
						context->status = SCSI_STATUS_CHECKCONDITION;
					}
					else
					{
						context->status = SCSI_STATUS_GOOD;
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
					// Additional sense length: the count of bytes after byte 7.
					// Eighteen byte fixed format sense means 10. It was left at
					// zero, which tells a compliant initiator that byte 12 -
					// the ASC it is being handed - is not there.
					context->data_buf[7] = 10;
					context->data_buf[12] = context->asc;
					context->status = SCSI_STATUS_GOOD;

					// Hand the sense data back, rather than going straight to
					// STATUS with GOOD and never sending it. Building the
					// eighteen bytes and then dropping them meant the host
					// could tell that a command had failed but never why -
					// every CHECK CONDITION in this file was effectively
					// reasonless.
					context->state = SCSI_STATE_DATAIN;
					context->seq = 0;

					// Sense is consumed by reading it. Leaving it latched
					// makes the next REQUEST SENSE describe an error that has
					// already been reported and dealt with.
					context->sensekey = SCSI_SENSEKEY_NOSENSE;
					context->asc = 0;
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

					// "Is there a disk" before "is that block on the disk".
					// The other way round, an absent image has a maximum size
					// of zero, so every LBA failed as out of range and the
					// medium-not-present case was unreachable.
					if (scsi_imagecheck(context))
					{
						context->sensekey = SCSI_SENSEKEY_NOTREADY;
						context->asc = SCSI_SASC_MEDIUMNOTPRESENT;
						context->status = SCSI_STATUS_CHECKCONDITION;
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
					if (scsi_take_deferred_write_error(context))
					{
						context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
						context->asc = SCSI_SASC_WRITEFAULT;
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
						context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
						context->asc = SCSI_SASC_UNRECOVEREDREADERROR;
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

					// Same ordering point as the read path: without this, an absent image
					// has a maximum size of zero so every LBA fails as out of range and
					// the medium-not-present case below is unreachable.
					if (scsi_imagecheck(context))
					{
						context->sensekey = SCSI_SENSEKEY_NOTREADY;
						context->asc = SCSI_SASC_MEDIUMNOTPRESENT;
						context->status = SCSI_STATUS_CHECKCONDITION;
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
					if (!scsi_imagecheck(context))
					{
						context->state = SCSI_STATE_DATAOUT;
						context->seq = 0;
					}
					else
					{
						// No image attached at that target/LUN, which is a
						// different thing from a media error and worth saying
						// so - HDOS can tell "no disk" from "bad disk".
						context->sensekey = SCSI_SENSEKEY_NOTREADY;
						context->asc = SCSI_SASC_MEDIUMNOTPRESENT;
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
						context->state = SCSI_STATE_STATUS;
						if (context->user_format(context))
						{
							context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
							context->asc = SCSI_SASC_WRITEFAULT;
							context->status = SCSI_STATUS_CHECKCONDITION;
						}
						else
						{
							context->status = SCSI_STATUS_GOOD;
						}
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
						// As on the write path: set the sense as well as the
						// status, or REQUEST SENSE describes whatever the last
						// command left behind.
						context->sensekey = SCSI_SENSEKEY_MEDIUMERROR;
						context->asc = SCSI_SASC_UNRECOVEREDREADERROR;
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
	// Flush FatFS metadata whenever we return to the status phase after writes.
	if (context->state == SCSI_STATE_BUSFREE)
	{
		for (int d = 0; d < SCSI_MAX_DISKS; ++d)
		{
			if (context->file[d])
				context->file[d]->Sync();
		}
	}
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
