// Host side stand-in for FatFS, for the tests.
//
// Only the surface src/emulation/scsi.cpp actually uses: seven functions, a
// handful of constants and FIL. Files live in memory, and every call can be
// made to fail on demand - which is the point, because the interesting
// behaviour in the disk cache is what it does when the card does not take the
// data.
//
// This header shadows src/fatfs/ff.h by being earlier on the include path. The
// emulator source is compiled unmodified against it.

#ifndef TESTS_SHIM_FF_H
#define TESTS_SHIM_FF_H

#include <stddef.h>
#include "integer.h"		// the real FatFS one - BYTE, UINT and friends

typedef unsigned long long FSIZE_t;

typedef enum {
	FR_OK = 0,
	FR_DISK_ERR,
	FR_INT_ERR,
	FR_NOT_READY,
	FR_NO_FILE,
	FR_DENIED,
	FR_EXIST,
	FR_INVALID_OBJECT,
	FR_WRITE_PROTECTED,
	FR_TIMEOUT
} FRESULT;

#define FA_READ				0x01
#define FA_WRITE			0x02
#define FA_CREATE_ALWAYS	0x08

// A FIL is just a handle into the fake file table plus a position.
typedef struct {
	int   id;			// index into the table, -1 when closed
	FSIZE_t fptr;		// current offset
} FIL;

#ifdef __cplusplus
extern "C" {
#endif

FRESULT f_open  (FIL* fp, const char* path, BYTE mode);
FRESULT f_close (FIL* fp);
FRESULT f_read  (FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write (FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek (FIL* fp, FSIZE_t ofs);
FRESULT f_sync  (FIL* fp);

FSIZE_t f_size_impl(FIL* fp);

#ifdef __cplusplus
}
#endif

// Real FatFS defines this as a macro over the FIL's own size field.
#define f_size(fp) f_size_impl(fp)

#endif
