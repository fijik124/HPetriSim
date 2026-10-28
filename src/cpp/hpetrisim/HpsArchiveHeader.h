#pragma once

#include <afx.h>

namespace hps
{
namespace legacy
{
// The Win32 document marker is six FF bytes. On x64, ReadCount would consume
// eight additional bytes, including real data from a Win32 file.
inline DWORD ReadDocumentCount(CArchive& archive)
{
	WORD shortCount = 0;
	archive >> shortCount;
	if (shortCount != 0xFFFF)
	{
		return shortCount;
	}
	DWORD count = 0;
	archive >> count;
	return count;
}

inline WORD ReadDocumentVersion(CArchive& archive)
{
	WORD version = 0;
	archive >> version;
	if (version == 0xFFFF)
	{
		// Also accept the eight extra FF bytes written by historical x64 builds.
		WORD remainingShort = 0;
		DWORD remainingLong = 0;
		archive >> remainingShort >> remainingLong;
		if (remainingShort != 0xFFFF || remainingLong != 0xFFFFFFFF)
		{
			AfxThrowArchiveException(CArchiveException::badSchema);
		}
		archive >> version;
	}
	return version;
}

inline void WriteDocumentHeader(CArchive& archive)
{
	// This canonical version-2 header is readable by the original Win32 reader.
	archive << static_cast<WORD>(0xFFFF) << static_cast<DWORD>(0xFFFFFFFF)
		<< static_cast<WORD>(2);
}
}
}
