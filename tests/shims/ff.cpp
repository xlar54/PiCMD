// In-memory FatFS stand-in. See ff.h and fakefs.h.

#include "ff.h"
#include "fakefs.h"
#include "fakeclock.h"

#include <map>
#include <vector>
#include <string>
#include <string.h>

namespace
{
	struct File
	{
		std::string name;
		std::vector<unsigned char> data;
		bool open;
	};

	std::vector<File> g_files;
	std::map<std::string, int> g_byName;

	int g_failWritesAfter  = -1;
	int g_shortWritesAfter = -1;
	unsigned g_shortWriteBytes = 0;
	int g_failReadsAfter   = -1;
	bool g_failSync        = false;
	bool g_failLseek       = false;

	unsigned g_writes = 0;
	unsigned g_reads  = 0;
	unsigned g_syncs  = 0;

	File* Lookup(FIL* fp)
	{
		if (!fp || fp->id < 0 || (size_t)fp->id >= g_files.size())
			return 0;
		return &g_files[fp->id];
	}

	// "have this many already gone through?" - shared by the fault switches.
	bool Triggered(int after, unsigned soFar)
	{
		return after >= 0 && soFar > (unsigned)after;
	}
}

namespace FakeFs
{
	void Reset()
	{
		g_files.clear();
		g_byName.clear();
		g_failWritesAfter = -1;
		g_shortWritesAfter = -1;
		g_shortWriteBytes = 0;
		g_failReadsAfter = -1;
		g_failSync = false;
		g_failLseek = false;
		g_writes = g_reads = g_syncs = 0;
	}

	void CreateFile(const std::string& name, size_t size, unsigned char fill)
	{
		File f;
		f.name = name;
		f.data.assign(size, fill);
		f.open = false;
		g_byName[name] = (int)g_files.size();
		g_files.push_back(f);
	}

	const std::vector<unsigned char>& Contents(const std::string& name)
	{
		static std::vector<unsigned char> empty;
		std::map<std::string, int>::iterator it = g_byName.find(name);
		if (it == g_byName.end())
			return empty;
		return g_files[it->second].data;
	}

	bool Exists(const std::string& name)
	{
		return g_byName.find(name) != g_byName.end();
	}

	void FailWritesAfter(int successes)   { g_failWritesAfter = successes; }
	void FailReadsAfter(int successes)    { g_failReadsAfter = successes; }
	void FailSync(bool fail)              { g_failSync = fail; }
	void FailLseek(bool fail)             { g_failLseek = fail; }

	void ShortWritesAfter(int successes, unsigned bytesToActuallyWrite)
	{
		g_shortWritesAfter = successes;
		g_shortWriteBytes = bytesToActuallyWrite;
	}

	unsigned WriteCallCount() { return g_writes; }
	unsigned ReadCallCount()  { return g_reads; }
	unsigned SyncCallCount()  { return g_syncs; }
}

extern "C"
{

FRESULT f_open(FIL* fp, const char* path, BYTE mode)
{
	if (!fp || !path)
		return FR_INT_ERR;

	std::string name(path);
	std::map<std::string, int>::iterator it = g_byName.find(name);

	if (it == g_byName.end())
	{
		if (!(mode & FA_CREATE_ALWAYS))
			return FR_NO_FILE;
		FakeFs::CreateFile(name, 0);
		it = g_byName.find(name);
	}
	else if (mode & FA_CREATE_ALWAYS)
	{
		g_files[it->second].data.clear();
	}

	fp->id = it->second;
	fp->fptr = 0;
	g_files[fp->id].open = true;
	return FR_OK;
}

FRESULT f_close(FIL* fp)
{
	File* f = Lookup(fp);
	if (!f)
		return FR_INVALID_OBJECT;
	f->open = false;
	fp->id = -1;
	return FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
{
	File* f = Lookup(fp);
	if (!f)
		return FR_INVALID_OBJECT;
	if (g_failLseek)
		return FR_DISK_ERR;
	fp->fptr = ofs;
	return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
	File* f = Lookup(fp);
	if (br) *br = 0;
	if (!f)
		return FR_INVALID_OBJECT;

	++g_reads;
	FakeClock::Advance(FakeClock::microsPerAccess);
	if (Triggered(g_failReadsAfter, g_reads))
		return FR_DISK_ERR;

	// Reads past the end return what is there, like the real thing.
	UINT avail = 0;
	if (fp->fptr < f->data.size())
		avail = (UINT)(f->data.size() - fp->fptr);
	UINT n = btr < avail ? btr : avail;

	if (n)
		memcpy(buff, &f->data[(size_t)fp->fptr], n);
	fp->fptr += n;
	if (br) *br = n;
	return FR_OK;
}

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
	File* f = Lookup(fp);
	if (bw) *bw = 0;
	if (!f)
		return FR_INVALID_OBJECT;

	++g_writes;
	FakeClock::Advance(FakeClock::microsPerAccess);

	if (Triggered(g_failWritesAfter, g_writes))
		return FR_DISK_ERR;

	UINT n = btw;
	bool shortWrite = Triggered(g_shortWritesAfter, g_writes);
	if (shortWrite)
	{
		// A full card reports success having written less than asked.
		n = g_shortWriteBytes;
		if (n > btw) n = btw;
	}

	if (f->data.size() < fp->fptr + n)
		f->data.resize((size_t)(fp->fptr + n), 0);
	if (n)
		memcpy(&f->data[(size_t)fp->fptr], buff, n);

	fp->fptr += n;
	if (bw) *bw = n;
	return FR_OK;
}

FRESULT f_sync(FIL* fp)
{
	File* f = Lookup(fp);
	if (!f)
		return FR_INVALID_OBJECT;
	++g_syncs;
	FakeClock::Advance(FakeClock::microsPerAccess);
	return g_failSync ? FR_DISK_ERR : FR_OK;
}

FSIZE_t f_size_impl(FIL* fp)
{
	File* f = Lookup(fp);
	return f ? (FSIZE_t)f->data.size() : 0;
}

} // extern "C"
