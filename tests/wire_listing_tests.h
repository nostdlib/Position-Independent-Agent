/**
 * @file wire_listing_tests.h
 * @brief Golden-vector tests for the v3 compact listing encoder
 *
 * @details Byte-exact mirrors of the C# suite's v3 vectors
 * (tests/C2.Tests/FileSystem/DirEntryTests.cs) — the two suites must agree
 * byte for byte, or the wire contract is broken. Pure logic: no syscalls,
 * no platform guard needed.
 */

#pragma once

#include "lib/runtime.h"
#include "tests.h"
#include "platform/fs/wire_listing.h"

class WireListingTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Wire Listing Tests...");

		RunTest(allPassed, &TestGoldenVectorsSuite, "Golden vectors suite");
		RunTest(allPassed, &TestVarintAndTimeSuite, "Varint and time conversion suite");
		RunTest(allPassed, &TestWtf8Suite, "WTF-8 suite");

		if (allPassed)
			LOG_INFO("All Wire Listing tests passed!");
		else
			LOG_ERROR("Some Wire Listing tests failed!");

		return allPassed;
	}

private:
	/// Positional: (name, isDirectory, isDrive, isHidden, isSystem, isReadOnly,
	/// size, creationTime, lastModifiedTime, type, volumeSerial) — DirectoryEntry's order.
	static DirectoryEntry MakeEntry(PCWCHAR name, BOOL isDirectory = false, BOOL isDrive = false,
									BOOL isHidden = false, BOOL isSystem = false, BOOL isReadOnly = false,
									UINT64 size = 0, UINT64 creationTime = 0, UINT64 lastModifiedTime = 0,
									UINT32 type = 0, UINT64 volumeSerial = 0)
	{
		DirectoryEntry entry;
		Memory::Zero(&entry, sizeof(entry));
		USIZE units = StringUtils::Length(name);
		if (units > 255)
			units = 255;
		for (USIZE i = 0; i < units; i++)
			entry.Name[i] = name[i];
		entry.IsDirectory = isDirectory;
		entry.IsDrive = isDrive;
		entry.IsHidden = isHidden;
		entry.IsSystem = isSystem;
		entry.IsReadOnly = isReadOnly;
		entry.Size = size;
		entry.CreationTime = creationTime;
		entry.LastModifiedTime = lastModifiedTime;
		entry.Type = type;
		entry.VolumeSerial = volumeSerial;
		return entry;
	}

	/// Byte-exact compare of an encoded packet against a golden frame.
	static BOOL ExpectFrame(Buffer<CHAR> &packet, const CHAR *expected, USIZE expectedLength, const CHAR *what)
	{
		BOOL passed = true;
		if (packet.Size != expectedLength)
		{
			LOG_ERROR("%s: frame is %zu bytes, expected %zu", what, packet.Size, expectedLength);
			return false;
		}
		for (USIZE i = 0; i < expectedLength; i++)
		{
			if ((UINT8)packet.Data[i] != (UINT8)expected[i])
			{
				LOG_ERROR("%s: byte %zu is %02X, expected %02X", what, i,
						  (UINT8)packet.Data[i], (UINT8)expected[i]);
				passed = false;
			}
		}
		if (!passed)
			for (USIZE i = 0; i < packet.Size; i++)
				LOG_ERROR("  [%zu] = %02X", i, (UINT8)packet.Data[i]);
		return passed;
	}

	/// Encode one entry and compare against a golden frame (header + single entry).
	static BOOL CheckSingleEntry(const DirectoryEntry &entry, ListingTimeEncoding enc,
								 const CHAR *expected, USIZE expectedLength, const CHAR *what)
	{
		Buffer<CHAR> packet;
		Result<VOID, Error> encoded = WireListing::Encode(
			Span<const DirectoryEntry>(&entry, 1), enc, packet);
		if (!encoded)
		{
			LOG_ERROR("%s: Encode failed: %e", what, encoded.Error());
			return false;
		}
		return ExpectFrame(packet, expected, expectedLength, what);
	}

	// Frame shape: [status 00000000][count u32le][format 03000000] + entries.
	// Entry shape: [nameLen u16le][name WTF-8][attrs u8][size LEB128][ctime u32][mtime u32]([serial u32] drive-only).

	static BOOL TestGoldenVectorsSuite()
	{
		BOOL allPassed = true;

		// --- Empty listing (12-byte header alone) ---
		{
			Buffer<CHAR> packet;
			Result<VOID, Error> encoded = WireListing::Encode(
				Span<const DirectoryEntry>(nullptr, 0), ListingTimeEncoding::ListingTime_UnixSeconds, packet);
			if (!encoded || packet.Size != 12)
			{
				LOG_ERROR("Empty: expected a 12-byte frame");
				allPassed = false;
			}
			else
			{
				static const CHAR Expected[12] = {0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0};
				allPassed &= ExpectFrame(packet, Expected, 12, "Empty");
			}
		}

		// --- Plain file, every field default (17-byte entry vs legacy 553) ---
		{
			static const CHAR Expected[29] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, // header: status, count 1, format 3
				0x05, 0x00,                         // nameLen 5
				0x61, 0x2E, 0x74, 0x78, 0x74,       // "a.txt"
				0x00,                               // attrs: no flags
				0x00,                               // size varint 0
				0x00, 0x00, 0x00, 0x00,             // ctime 0
				0x00, 0x00, 0x00, 0x00};            // mtime 0
			allPassed &= CheckSingleEntry(MakeEntry(L"a.txt"), ListingTimeEncoding::ListingTime_UnixSeconds,
										  Expected, sizeof(Expected), "PlainFile");
		}

		// --- Multi-byte varint + unix mtime 1700000000 (0x6553F100 LE) ---
		{
			static const CHAR Expected[32] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x07, 0x00,
				0x62, 0x69, 0x67, 0x2E, 0x62, 0x69, 0x6E, // "big.bin"
				0x00,                               // attrs
				(CHAR)0xAC, 0x02,                   // size 300 (LEB128)
				0x00, 0x00, 0x00, 0x00,             // ctime 0
				0x00, (CHAR)0xF1, 0x53, 0x65};      // mtime 1700000000
			allPassed &= CheckSingleEntry(MakeEntry(L"big.bin", false, false, false, false, false, 300, 0, 1700000000),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  Expected, sizeof(Expected), "VarintAndTime");
		}

		// --- Drive entry: attrs 0x63 (dir|drive|type 3 << 5) + trailing serial u32 ---
		{
			static const CHAR Expected[31] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x03, 0x00,
				0x43, 0x3A, 0x5C,                   // "C:\"
				0x63,                               // attrs
				0x00,                               // size 0
				0x00, 0x00, 0x00, 0x00,             // ctime 0
				0x00, 0x00, 0x00, 0x00,             // mtime 0
				(CHAR)0xA2, 0x59, 0x41, (CHAR)0xF4}; // serial 0xF44159A2 LE
			allPassed &= CheckSingleEntry(MakeEntry(L"C:\\", true, true, false, false, false, 0, 0, 0, 3, 0xF44159A2),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  Expected, sizeof(Expected), "Drive");
		}

		// --- WPD token fold: a 64-bit hash serial folds high^low into the u32 wire
		//     field (0x123456789ABCDEF0 ^ 0x12345678 = 0x88888888) ---
		{
			static const CHAR WpdFold[38] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x0A, 0x00,                                        // nameLen 10
				':', ':', 'm', 't', 'p', '-', '0', '1', '2', '3', // "::mtp-0123"
				0x43,                                              // attrs: dir|drive, type 2 << 5
				0x00,                                              // size 0
				0x00, 0x00, 0x00, 0x00,                            // ctime 0
				0x00, 0x00, 0x00, 0x00,                            // mtime 0
				(CHAR)0x88, (CHAR)0x88, (CHAR)0x88, (CHAR)0x88};  // folded serial LE
			allPassed &= CheckSingleEntry(MakeEntry(L"::mtp-0123", true, true, false, false, false, 0, 0, 0, 2, 0x123456789ABCDEF0ULL),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  WpdFold, sizeof(WpdFold), "WpdSerialFold");
		}

		// --- Flag bits: read-only, directory ---
		{
			static const CHAR ReadOnly[30] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x06, 0x00, 0x72, 0x6F, 0x2E, 0x64, 0x61, 0x74, // "ro.dat"
				0x10, 0x01,                       // attrs RO, size varint 1
				0, 0, 0, 0, 0, 0, 0, 0};
			allPassed &= CheckSingleEntry(MakeEntry(L"ro.dat", false, false, false, false, true, 1),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  ReadOnly, sizeof(ReadOnly), "ReadOnlyFlag");

			static const CHAR Dir4096[31] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x06, 0x00, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, // "Photos"
				0x01, (CHAR)0x80, 0x20,           // attrs dir, size 4096 (LEB128)
				0, 0, 0, 0, 0, 0, 0, 0};
			allPassed &= CheckSingleEntry(MakeEntry(L"Photos", true, false, false, false, false, 4096),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  Dir4096, sizeof(Dir4096), "DirectoryFlag");
		}

		// --- Zero-length name entry is still emitted (the C2 drops it) ---
		{
			static const CHAR Expected[24] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x00, 0x00,                         // nameLen 0
				0x00, 0x00,                         // attrs, size 0
				0, 0, 0, 0, 0, 0, 0, 0};
			allPassed &= CheckSingleEntry(MakeEntry(L""), ListingTimeEncoding::ListingTime_UnixSeconds,
										  Expected, sizeof(Expected), "EmptyName");
		}

		return allPassed;
	}

	static BOOL TestVarintAndTimeSuite()
	{
		BOOL allPassed = true;

		// --- Varint range: 2^32+1 (5 bytes) and u64 max (10 bytes) ---
		{
			static const CHAR CrossU32[29] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x01, 0x00, 0x66,                   // nameLen 1, "f"
				0x00,                               // attrs
				(CHAR)0x81, (CHAR)0x80, (CHAR)0x80, (CHAR)0x80, 0x10, // 2^32+1
				0, 0, 0, 0, 0, 0, 0, 0};
			allPassed &= CheckSingleEntry(MakeEntry(L"f", false, false, false, false, false, 0x100000001ull),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  CrossU32, sizeof(CrossU32), "Varint2p32");

			static const CHAR U64Max[34] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x01, 0x00, 0x66,
				0x00,
				(CHAR)0xFF, (CHAR)0xFF, (CHAR)0xFF, (CHAR)0xFF, (CHAR)0xFF,
				(CHAR)0xFF, (CHAR)0xFF, (CHAR)0xFF, (CHAR)0xFF, 0x01, // u64 max
				0, 0, 0, 0, 0, 0, 0, 0};
			allPassed &= CheckSingleEntry(MakeEntry(L"f", false, false, false, false, false, 0xFFFFFFFFFFFFFFFFull),
										  ListingTimeEncoding::ListingTime_UnixSeconds,
										  U64Max, sizeof(U64Max), "VarintU64Max");
		}

		// --- FILETIME kind: the epoch round-trip and the clamps ---
		{
			BOOL passed = true;
			if (WireListing::ToUnixSeconds(0, ListingTimeEncoding::ListingTime_FileTime) != 0)
			{
				LOG_ERROR("FileTime 0 must stay 0");
				passed = false;
			}
			constexpr UINT64 UnixEpochFileTime = 11644473600ull * 10000000ull;
			if (WireListing::ToUnixSeconds(UnixEpochFileTime, ListingTimeEncoding::ListingTime_FileTime) != 0)
			{
				LOG_ERROR("FileTime at the 1970 epoch must clamp to 0");
				passed = false;
			}
			if (WireListing::ToUnixSeconds(UnixEpochFileTime + 1700000000ull * 10000000ull,
										   ListingTimeEncoding::ListingTime_FileTime) != 1700000000u)
			{
				LOG_ERROR("FileTime for unix 1700000000 must convert back exactly");
				passed = false;
			}
			if (WireListing::ToUnixSeconds(0xFFFFFFFFFFFFFFFFull, ListingTimeEncoding::ListingTime_FileTime) != 0xFFFFFFFFu)
			{
				LOG_ERROR("Far-future FILETIME must saturate, not wrap");
				passed = false;
			}
			if (WireListing::ToUnixSeconds(0x100000000ull, ListingTimeEncoding::ListingTime_UnixSeconds) != 0xFFFFFFFFu)
			{
				LOG_ERROR("Far-future unix seconds must saturate");
				passed = false;
			}
			if (WireListing::ToUnixSeconds((UINT64)(INT64)-1, ListingTimeEncoding::ListingTime_UnixSeconds) != 0)
			{
				LOG_ERROR("Negative unix mtime must map to unknown (0), not saturate to 2106");
				passed = false;
			}
			allPassed &= passed;
		}

		// --- FILETIME-kind entry bytes match the unix-kind entry for the same instant ---
		{
			static const CHAR Expected[32] = {
				0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
				0x07, 0x00, 0x62, 0x69, 0x67, 0x2E, 0x62, 0x69, 0x6E,
				0x00, (CHAR)0xAC, 0x02,
				0x00, 0x00, 0x00, 0x00,
				0x00, (CHAR)0xF1, 0x53, 0x65};
			allPassed &= CheckSingleEntry(
				MakeEntry(L"big.bin", false, false, false, false, false, 300, 0,
						  (11644473600ull + 1700000000ull) * 10000000ull),
				ListingTimeEncoding::ListingTime_FileTime,
				Expected, sizeof(Expected), "FileTimeKindSameInstant");
		}

		// --- Release() hands over exactly Size bytes (the handler captures Size first) ---
		{
			DirectoryEntry entry = MakeEntry(L"a.txt");
			Buffer<CHAR> packet;
			Result<VOID, Error> encoded = WireListing::Encode(
				Span<const DirectoryEntry>(&entry, 1), ListingTimeEncoding::ListingTime_UnixSeconds, packet);
			BOOL passed = encoded.IsOk();
			USIZE sizeBeforeRelease = packet.Size;
			CHAR *released = packet.Release();
			if (!released || sizeBeforeRelease != 29)
			{
				LOG_ERROR("Release precondition: size %zu (expected 29)", sizeBeforeRelease);
				passed = false;
			}
			if (released && released[8] != 0x03)
			{
				LOG_ERROR("Released frame's format word must be 3 at offset 8");
				passed = false;
			}
			delete[] released;
			allPassed &= passed;
		}

		return allPassed;
	}

	static BOOL TestWtf8Suite()
	{
		BOOL allPassed = true;

		// --- WTF-8 names: BMP non-ASCII, astral, lone surrogates, escape unit ---
		{
			// Four entries in one frame — café.txt (21 B), U+1F600 (16 B),
			// lone D800 (15 B), lone DC80 (15 B): 12 + 67 = 79 bytes.
			CHAR frame[79];
			frame[0] = 0; frame[1] = 0; frame[2] = 0; frame[3] = 0;
			frame[4] = 4; frame[5] = 0; frame[6] = 0; frame[7] = 0;
			frame[8] = 3; frame[9] = 0; frame[10] = 0; frame[11] = 0;
			USIZE o = 12;
			// café.txt: nameLen 9 + 9 name bytes
			frame[o++] = 0x09; frame[o++] = 0x00;
			frame[o++] = 0x63; frame[o++] = 0x61; frame[o++] = 0x66; frame[o++] = (CHAR)0xC3; frame[o++] = (CHAR)0xA9; frame[o++] = 0x2E; frame[o++] = 0x74; frame[o++] = 0x78; frame[o++] = 0x74;
			for (UINT32 i = 0; i < 10; i++) frame[o++] = 0; // attrs + size + times
			// U+1F600: nameLen 4 + 4-byte sequence
			frame[o++] = 0x04; frame[o++] = 0x00;
			frame[o++] = (CHAR)0xF0; frame[o++] = 0x9F; frame[o++] = (CHAR)0x98; frame[o++] = (CHAR)0x80;
			for (UINT32 i = 0; i < 10; i++) frame[o++] = 0;
			// lone D800: nameLen 3 + ED A0 80
			frame[o++] = 0x03; frame[o++] = 0x00;
			frame[o++] = (CHAR)0xED; frame[o++] = (CHAR)0xA0; frame[o++] = (CHAR)0x80;
			for (UINT32 i = 0; i < 10; i++) frame[o++] = 0;
			// lone DC80: nameLen 3 + ED B2 80
			frame[o++] = 0x03; frame[o++] = 0x00;
			frame[o++] = (CHAR)0xED; frame[o++] = (CHAR)0xB2; frame[o++] = (CHAR)0x80;
			for (UINT32 i = 0; i < 10; i++) frame[o++] = 0;

			DirectoryEntry names[4] = {
				MakeEntry(L"café.txt"),
				MakeEntry(L"\U0001F600"),
				MakeEntry(L"\xD800"),
				MakeEntry(L"\xDC80"),
			};
			Buffer<CHAR> packet;
			Result<VOID, Error> encoded = WireListing::Encode(
				Span<const DirectoryEntry>(names, 4), ListingTimeEncoding::ListingTime_UnixSeconds, packet);
			BOOL passed = true;
			if (!encoded)
			{
				LOG_ERROR("Wtf8Names: Encode failed: %e", encoded.Error());
				passed = false;
			}
			else
			{
				passed = ExpectFrame(packet, frame, o, "Wtf8Names");
			}
			allPassed &= passed;
		}

		// --- WTF8Length agrees with ToWTF8 on every name shape (the sizing contract) ---
		{
			BOOL passed = true;
			// the 'b' must be b: a bare 'b' after \xD800 would be swallowed
			// into the hex escape (\xD800b is one code unit, not 'a' + D800 + 'b')
			PCWCHAR names[] = {L"plain.txt", L"café.txt", L"\U0001F600", L"\xD800", L"\xDC80",
							   L"a\xD800\u0062", L"\xD800\xDC00"}; // lone surrogate inside a name + a real pair
			CHAR staging[WireListing::MaxNameBytes + 4];
			for (UINT32 i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			{
				Span<const WCHAR> input(names[i], StringUtils::Length(names[i]));
				USIZE written = UTF16::ToWTF8(input, Span<CHAR>(staging, sizeof(staging)));
				USIZE sized = UTF16::WTF8Length(input);
				if (written != sized)
				{
					LOG_ERROR("WTF8Length (%zu) != ToWTF8 (%zu) on vector %u", sized, written, i);
					passed = false;
				}
			}
			allPassed &= passed;
		}

		// --- ToUTF8Lossless regression: escape units still become RAW BYTES there ---
		{
			// The POSIX openat() byte-exactness contract — this must NOT become WTF-8.
			WCHAR unit[1] = {0xDC80};
			CHAR out[8];
			USIZE written = UTF16::ToUTF8Lossless(Span<const WCHAR>(unit, 1), Span<CHAR>(out, sizeof(out)));
			if (written != 1 || (UINT8)out[0] != 0x80)
			{
				LOG_ERROR("ToUTF8Lossless must unescape U+DC80 to the raw byte 0x80 (got %zu bytes)", written);
				allPassed = false;
			}
		}

		return allPassed;
	}
};
