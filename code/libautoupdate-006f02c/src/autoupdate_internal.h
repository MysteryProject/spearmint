/*
 * Copyright (c) 2020 Minqi Pan et al.
 *
 * This file is part of libautoupdate, distributed under the MIT License
 * For full terms see the included LICENSE file
 */

#ifndef AUTOUPDATE_INTERNAL_H_A40E122A
#define AUTOUPDATE_INTERNAL_H_A40E122A

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32

#pragma pack(push, 1)
struct ZIPLocalFileHeader
{
	uint32_t signature;
	uint16_t versionNeededToExtract; // unsupported
	uint16_t generalPurposeBitFlag; // unsupported
	uint16_t compressionMethod;
	uint16_t lastModFileTime;
	uint16_t lastModFileDate;
	uint32_t crc32;
	uint32_t compressedSize;
	uint32_t uncompressedSize;
	uint16_t fileNameLength;
	uint16_t extraFieldLength; // unsupported
};
#pragma pack(pop)

wchar_t* autoupdate_tmpdir();
wchar_t* autoupdate_tmpf(wchar_t *tmpdir, const char *ext_name);
short autoupdate_should_proceed_24_hours(int argc, wchar_t *wargv[], short will_write);

#else

char* autoupdate_tmpdir();
char* autoupdate_tmpf(char *tmpdir, const char *ext_name);
short autoupdate_should_proceed_24_hours(int argc, char *argv[], short will_write);
	
#endif // _WIN32

short autoupdate_should_proceed();
int autoupdate_exepath(char* buffer, size_t* size);

#endif /* end of include guard: AUTOUPDATE_INTERNAL_H_A40E122A */
