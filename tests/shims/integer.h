// FatFS integer types, embedded flavour, for the host tests.
//
// The real src/fatfs/integer.h takes an "#ifdef _WIN32 -> #include <windows.h>"
// branch meant for FatFS's own development environment. Compiling the emulator
// on a Windows host trips that, and windows.h then collides with uspi's
// `boolean`. This is the embedded branch of that file with no such branch in
// it, and nothing else.

#ifndef _FF_INTEGER
#define _FF_INTEGER

typedef int				INT;
typedef unsigned int	UINT;

typedef signed char		CHAR;
typedef unsigned char	BYTE;

typedef short			SHORT;
typedef unsigned short	WORD;
typedef unsigned short	WCHAR;

typedef long			LONG;
typedef unsigned long	DWORD;

typedef unsigned long long QWORD;

#endif
