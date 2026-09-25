#ifndef PICMD_STARTUPIMAGE_H
#define PICMD_STARTUPIMAGE_H

#include <string.h>
#include <strings.h>

inline bool IsCMDHDImageName(const char* name)
{
	const char* ext = strrchr(name, '.');
	return ext && (!strcasecmp(ext, ".dhd") || !strcasecmp(ext, ".img"));
}

// Entries are sorted by the browser. A configured name takes precedence;
// never silently substitute another disk when that name is missing.
template<class Entries>
int FindStartupImage(const Entries& entries, const char* configured, unsigned directoryFlag)
{
	for (unsigned i = 0; i < entries.size(); ++i)
	{
		const auto& file = entries[i].filImage;
		if (!(file.fattrib & directoryFlag) && IsCMDHDImageName(file.fname) &&
			(!configured[0] || !strcasecmp(file.fname, configured)))
			return (int)i;
	}
	return -1;
}

#endif
