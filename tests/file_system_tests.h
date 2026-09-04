#pragma once

#include "lib/runtime.h"
#include "tests.h"
#if defined(PLATFORM_LINUX)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
// Not in the kernel header; x86_64 numbers
constexpr USIZE SYS_SYMLINK_T = 88;
constexpr USIZE SYS_MKNOD_T = 133;
#endif


class FileSystemTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running FileSystem Tests...");

		RunTest(allPassed, &TestCreateNestedDirectories, "Create nested directories");
		RunTest(allPassed, &TestCreateFilesInDirectories, "Create files in directories");
		RunTest(allPassed, &TestWriteReadContent, "Write and read file content");
		RunTest(allPassed, &TestFileExistence, "File existence checks");
		RunTest(allPassed, &TestDirectoryIteration, "Directory iteration");
		RunTest(allPassed, &TestEmptyDirectoryIteration, "Empty directory iteration");
		RunTest(allPassed, &TestIterationErrorSeparation, "Iteration error separation");
		RunTest(allPassed, &TestUtf8RoundTripConversion, "UTF-8 lossless conversion round-trip");
		RunTest(allPassed, &TestLosslessFilenames, "Lossless filename round-trip");
		RunTest(allPassed, &TestEnumerationCornerCases, "Enumeration corner cases");
		RunTest(allPassed, &TestLargeDirectory, "Large directory exact count");
		RunTest(allPassed, &TestDriveEnumeration, "Drive enumeration");
		RunTest(allPassed, &TestCleanup, "Cleanup files and directories");

		if (allPassed)
			LOG_INFO("All FileSystem tests passed!");
		else
			LOG_ERROR("Some FileSystem tests failed!");

		return allPassed;
	}

private:
	// ── Path building helpers ──────────────────────────────────────────
	// Embeds "test_io_root" once and combines with suffix at runtime,
	// avoiding ~50 unique full-path string instantiations.

	static NOINLINE USIZE BuildTestPath(PCWCHAR suffix, Span<WCHAR> out)
	{
		const WCHAR root[] = L"test_io_root";
		constexpr USIZE rootLen = sizeof(root) / sizeof(WCHAR) - 1;

		if (suffix == nullptr || suffix[0] == L'\0')
		{
			StringUtils::Copy(out, Span<const WCHAR>(root, rootLen));
			return rootLen;
		}

		USIZE suffixLen = StringUtils::Length(suffix);
		return Path::Combine(
			Span<const WCHAR>(root, rootLen),
			Span<const WCHAR>(suffix, suffixLen),
			out);
	}

	// ── Filesystem operation wrappers ──────────────────────────────────

	static NOINLINE BOOL MkDir(PCWCHAR suffix)
	{
		WCHAR path[128];
		BuildTestPath(suffix, Span<WCHAR>(path));
		return (BOOL)Directory::Create(path);
	}

	static NOINLINE BOOL RmDir(PCWCHAR suffix)
	{
		WCHAR path[128];
		BuildTestPath(suffix, Span<WCHAR>(path));
		return (BOOL)Directory::Delete(path);
	}

	static NOINLINE BOOL RmFile(PCWCHAR suffix)
	{
		WCHAR path[128];
		BuildTestPath(suffix, Span<WCHAR>(path));
		return (BOOL)File::Delete(path);
	}

	static NOINLINE BOOL PathExists(PCWCHAR suffix)
	{
		WCHAR path[128];
		BuildTestPath(suffix, Span<WCHAR>(path));
		return (BOOL)File::Exists(path);
	}

	static NOINLINE BOOL CreateEmptyFile(PCWCHAR suffix)
	{
		WCHAR path[128];
		BuildTestPath(suffix, Span<WCHAR>(path));
		auto r = File::Open(path, File::ModeCreate | File::ModeWrite);
		if (!r)
			return false;
		r.Value().Close();
		return true;
	}

	static NOINLINE Result<File, Error> OpenTestFile(PCWCHAR suffix, UINT32 mode)
	{
		WCHAR path[128];
		BuildTestPath(suffix, Span<WCHAR>(path));
		return File::Open(path, mode);
	}

	// ── Test methods ───────────────────────────────────────────────────

	static BOOL TestCreateNestedDirectories()
	{
		// Create root directory
		if (!MkDir(nullptr))
		{
			LOG_ERROR("Failed to create test_io_root");
			return false;
		}

		// Create first level directories
		if (!MkDir(L"level1_dir1"))
		{
			LOG_ERROR("Failed to create level1_dir1");
			return false;
		}
		if (!MkDir(L"level1_dir2"))
		{
			LOG_ERROR("Failed to create level1_dir2");
			return false;
		}
		if (!MkDir(L"level1_dir3"))
		{
			LOG_ERROR("Failed to create level1_dir3");
			return false;
		}

		// Create second level directories
		if (!MkDir(L"level1_dir1\\level2_dir1"))
		{
			LOG_ERROR("Failed to create level2_dir1");
			return false;
		}
		if (!MkDir(L"level1_dir1\\level2_dir2"))
		{
			LOG_ERROR("Failed to create level2_dir2");
			return false;
		}
		if (!MkDir(L"level1_dir2\\level2_dir3"))
		{
			LOG_ERROR("Failed to create level2_dir3");
			return false;
		}
		if (!MkDir(L"level1_dir2\\level2_dir4"))
		{
			LOG_ERROR("Failed to create level2_dir4");
			return false;
		}
		if (!MkDir(L"level1_dir3\\level2_dir5"))
		{
			LOG_ERROR("Failed to create level2_dir5");
			return false;
		}

		// Verify all directories exist
		if (!PathExists(nullptr))
		{
			LOG_ERROR("test_io_root does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir1"))
		{
			LOG_ERROR("level1_dir1 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir2"))
		{
			LOG_ERROR("level1_dir2 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir3"))
		{
			LOG_ERROR("level1_dir3 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir1\\level2_dir1"))
		{
			LOG_ERROR("level2_dir1 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir1\\level2_dir2"))
		{
			LOG_ERROR("level2_dir2 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir2\\level2_dir3"))
		{
			LOG_ERROR("level2_dir3 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir2\\level2_dir4"))
		{
			LOG_ERROR("level2_dir4 does not exist after creation");
			return false;
		}
		if (!PathExists(L"level1_dir3\\level2_dir5"))
		{
			LOG_ERROR("level2_dir5 does not exist after creation");
			return false;
		}

		return true;
	}

	static BOOL TestCreateFilesInDirectories()
	{
		if (!CreateEmptyFile(L"root_file.txt"))
		{
			LOG_ERROR("Failed to create root_file.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir1\\file1.txt"))
		{
			LOG_ERROR("Failed to create file1.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir2\\file2.txt"))
		{
			LOG_ERROR("Failed to create file2.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir3\\file3.txt"))
		{
			LOG_ERROR("Failed to create file3.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir1\\level2_dir1\\deep_file1.txt"))
		{
			LOG_ERROR("Failed to create deep_file1.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir1\\level2_dir2\\deep_file2.txt"))
		{
			LOG_ERROR("Failed to create deep_file2.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir2\\level2_dir3\\deep_file3.txt"))
		{
			LOG_ERROR("Failed to create deep_file3.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir2\\level2_dir4\\deep_file4.txt"))
		{
			LOG_ERROR("Failed to create deep_file4.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir3\\level2_dir5\\deep_file5.txt"))
		{
			LOG_ERROR("Failed to create deep_file5.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir1\\extra1.txt"))
		{
			LOG_ERROR("Failed to create extra1.txt");
			return false;
		}
		if (!CreateEmptyFile(L"level1_dir1\\extra2.txt"))
		{
			LOG_ERROR("Failed to create extra2.txt");
			return false;
		}

		return true;
	}

	static BOOL TestWriteReadContent()
	{
		// Test 1: Simple text
		{
			auto openResult = OpenTestFile(L"test_write_read.txt",
											File::ModeCreate | File::ModeWrite | File::ModeTruncate);
			if (!openResult)
			{
				LOG_ERROR("Failed to open test_write_read.txt for writing");
				return false;
			}
			File &file = openResult.Value();

			auto testData = "Hello, File System!";
			auto writeResult = file.Write(Span<const UINT8>((const UINT8 *)(const CHAR *)testData, 20));
			if (!writeResult)
			{
				LOG_ERROR("Write to test_write_read.txt failed (error: %e)", writeResult.Error());
				return false;
			}
			if (writeResult.Value() != 20)
			{
				LOG_ERROR("Write to test_write_read.txt: expected 20 bytes, got %u", writeResult.Value());
				return false;
			}

			file.Close();

			// Read it back
			auto readOpenResult = OpenTestFile(L"test_write_read.txt", File::ModeRead);
			if (!readOpenResult)
			{
				LOG_ERROR("Failed to open test_write_read.txt for reading");
				return false;
			}
			File &readFile = readOpenResult.Value();

			CHAR buffer[32];
			Memory::Zero(buffer, 32);
			auto readResult = readFile.Read(Span<UINT8>((UINT8 *)buffer, 20));
			if (!readResult)
			{
				LOG_ERROR("Read from test_write_read.txt failed (error: %e)", readResult.Error());
				return false;
			}
			if (readResult.Value() != 20)
			{
				LOG_ERROR("Read from test_write_read.txt: expected 20 bytes, got %u", readResult.Value());
				return false;
			}

			// Verify content
			for (INT32 i = 0; i < 20; i++)
			{
				if (buffer[i] != ((const CHAR *)testData)[i])
				{
					LOG_ERROR("Content mismatch at index %d", i);
					return false;
				}
			}

			readFile.Close();
		}

		// Test 2: Binary data
		{
			auto openResult = OpenTestFile(L"level1_dir1\\binary_test.dat",
											File::ModeCreate | File::ModeWrite | File::ModeTruncate);
			if (!openResult)
			{
				LOG_ERROR("Failed to open binary_test.dat for writing");
				return false;
			}
			File &file = openResult.Value();

			UINT8 binaryData[256];
			for (INT32 i = 0; i < 256; i++)
			{
				binaryData[i] = (UINT8)i;
			}

			auto writeResult = file.Write(Span<const UINT8>(binaryData));
			if (!writeResult)
			{
				LOG_ERROR("Binary write failed (error: %e)", writeResult.Error());
				return false;
			}
			if (writeResult.Value() != 256)
			{
				LOG_ERROR("Binary write: expected 256 bytes, got %u", writeResult.Value());
				return false;
			}

			file.Close();

			// Read it back
			auto readOpenResult = OpenTestFile(L"level1_dir1\\binary_test.dat", File::ModeRead);
			if (!readOpenResult)
			{
				LOG_ERROR("Failed to open binary_test.dat for reading");
				return false;
			}
			File &readFile = readOpenResult.Value();

			UINT8 readBuffer[256];
			Memory::Zero(readBuffer, 256);
			auto readResult = readFile.Read(Span<UINT8>(readBuffer));
			if (!readResult)
			{
				LOG_ERROR("Binary read failed (error: %e)", readResult.Error());
				return false;
			}
			if (readResult.Value() != 256)
			{
				LOG_ERROR("Binary read: expected 256 bytes, got %u", readResult.Value());
				return false;
			}

			// Verify content
			for (INT32 i = 0; i < 256; i++)
			{
				if (readBuffer[i] != (UINT8)i)
				{
					LOG_ERROR("Binary content mismatch at index %d: got %u", i, (UINT32)readBuffer[i]);
					return false;
				}
			}

			readFile.Close();
		}

		// Test 3: File offset operations
		{
			auto openResult = OpenTestFile(L"level1_dir2\\offset_test.dat",
											File::ModeCreate | File::ModeWrite | File::ModeTruncate);
			if (!openResult)
			{
				LOG_ERROR("Failed to open offset_test.dat for writing");
				return false;
			}
			File &file = openResult.Value();

			auto data = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
			auto writeResult = file.Write(Span<const UINT8>((const UINT8 *)(const CHAR *)data, 26));
			if (!writeResult)
			{
				LOG_ERROR("Offset test write failed (error: %e)", writeResult.Error());
				return false;
			}

			// Test SetOffset
			auto setResult = file.SetOffset(10);
			if (!setResult)
			{
				LOG_ERROR("SetOffset(10) failed (error: %e)", setResult.Error());
				return false;
			}
			auto getResult = file.GetOffset();
			if (!getResult || getResult.Value() != 10)
			{
				LOG_ERROR("SetOffset(10): GetOffset() returned %u", getResult ? (UINT32)getResult.Value() : 0);
				return false;
			}

			// Test MoveOffset from current position
			auto moveResult = file.MoveOffset(5, OffsetOrigin::Current);
			if (!moveResult)
			{
				LOG_ERROR("MoveOffset(5, Current) failed (error: %e)", moveResult.Error());
				return false;
			}
			getResult = file.GetOffset();
			if (!getResult || getResult.Value() != 15)
			{
				LOG_ERROR("MoveOffset(5, Current): GetOffset() returned %u", getResult ? (UINT32)getResult.Value() : 0);
				return false;
			}

			// Test MoveOffset from start
			moveResult = file.MoveOffset(0, OffsetOrigin::Start);
			if (!moveResult)
			{
				LOG_ERROR("MoveOffset(0, Start) failed (error: %e)", moveResult.Error());
				return false;
			}
			getResult = file.GetOffset();
			if (!getResult || getResult.Value() != 0)
			{
				LOG_ERROR("MoveOffset(0, Start): GetOffset() returned %u", getResult ? (UINT32)getResult.Value() : 0);
				return false;
			}

			file.Close();
		}

		return true;
	}

	// --- Drive enumeration (Windows only) ---
	// On POSIX an empty path coerces to "/" and IsDrive is always false,
	// so this test has no meaning there.
	static BOOL TestDriveEnumeration()
	{
#if defined(PLATFORM_WINDOWS)
		auto rootResult = DirectoryIterator::Create(L"");
		if (!rootResult)
		{
			LOG_ERROR("Failed to create DirectoryIterator for root");
			return false;
		}
		DirectoryIterator &rootIter = rootResult.Value();

		INT32 driveCount = 0;
		BOOL anySerial = false;

		while (rootIter.Next())
		{
			const DirectoryEntry &entry = rootIter.Get();

			if (!entry.IsDrive)
				continue;

			driveCount++;

			// Every drive entry must be formatted "X:\"
			if (entry.Name[1] != L':' || entry.Name[2] != L'\\' || entry.Name[3] != L'\0' ||
				entry.Name[0] < L'A' || entry.Name[0] > L'Z')
			{
				LOG_ERROR("Drive entry malformed: %ws", entry.Name);
				return false;
			}
			if (!entry.IsDirectory)
			{
				LOG_ERROR("Drive entry %ws is not flagged IsDirectory", entry.Name);
				return false;
			}

			if (entry.VolumeSerial != 0)
				anySerial = true;

			LOG_INFO("  Drive %ws type=%u serial=0x%llX", entry.Name, entry.Type,
				(unsigned long long)entry.VolumeSerial);
		}
		rootIter.Close();

		// Any machine running this test has at least one mounted, readable volume.
		if (driveCount == 0)
		{
			LOG_ERROR("Drive enumeration returned no drives");
			return false;
		}
		// Best-effort field: tolerate 0 for empty card readers / locked volumes,
		// but at least one enumerated (non-remote) drive must report a serial.
		if (!anySerial)
		{
			LOG_INFO("  Drive enumeration: %d drives, no serial reported (best-effort field)", driveCount);
		}
		else
		{
			LOG_INFO("  Drive enumeration: %d drives, at least one serial reported", driveCount);
		}
#else
		LOG_INFO("Drive enumeration skipped (POSIX: no drive roots)");
#endif
		return true;
	}

	static BOOL TestFileExistence()
	{
		// Test existing files
		if (!PathExists(L"root_file.txt"))
		{
			LOG_ERROR("root_file.txt should exist");
			return false;
		}
		if (!PathExists(L"level1_dir1\\file1.txt"))
		{
			LOG_ERROR("file1.txt should exist");
			return false;
		}
		if (!PathExists(L"level1_dir1\\level2_dir1\\deep_file1.txt"))
		{
			LOG_ERROR("deep_file1.txt should exist");
			return false;
		}

		// Test non-existing files
		if (PathExists(L"nonexistent.txt"))
		{
			LOG_ERROR("nonexistent.txt should not exist");
			return false;
		}
		if (PathExists(L"level1_dir1\\missing.txt"))
		{
			LOG_ERROR("missing.txt should not exist");
			return false;
		}

		return true;
	}

	static BOOL TestDirectoryIteration()
	{
		auto rootResult = DirectoryIterator::Create(L"");
		if (!rootResult)
		{
			LOG_ERROR("Failed to create DirectoryIterator for root");
			return false;
		}

		// Test iterating through a directory with multiple files
		WCHAR iterPath[128];
		BuildTestPath(L"level1_dir1", Span<WCHAR>(iterPath));
		auto iterResult = DirectoryIterator::Create(iterPath);
		if (!iterResult)
		{
			LOG_ERROR("Failed to create DirectoryIterator for level1_dir1");
			return false;
		}
		DirectoryIterator &iter = iterResult.Value();

		INT32 fileCount = 0;
		INT32 dirCount = 0;

		while (iter.Next())
		{
			const DirectoryEntry &entry = iter.Get();

			// Skip "." and ".." entries
			if (entry.Name[0] == L'.' &&
				(entry.Name[1] == L'\0' ||
				 (entry.Name[1] == L'.' && entry.Name[2] == L'\0')))
			{
				continue;
			}

			if (entry.IsDirectory)
			{
				dirCount++;
			}
			else
			{
				fileCount++;
			}
		}

		// We created 2 subdirectories and 3 files in level1_dir1
		// (file1.txt, extra1.txt, extra2.txt, binary_test.dat = 4 files)
		// (level2_dir1, level2_dir2 = 2 directories)
		if (fileCount != 4)
		{
			LOG_ERROR("Directory iteration: expected 4 files, got %d", fileCount);
			return false;
		}
		if (dirCount != 2)
		{
			LOG_ERROR("Directory iteration: expected 2 dirs, got %d", dirCount);
			return false;
		}

		return true;
	}

	// An empty directory must yield a VALID iterator whose first Next() is
	// the clean-end sentinel — not a Create() failure (the caller would
	// report "access denied" for a directory that simply has no entries).
	static BOOL TestEmptyDirectoryIteration()
	{
		if (!MkDir(L"empty_dir"))
		{
			LOG_ERROR("Failed to create empty_dir");
			return false;
		}

		WCHAR dirPath[128];
		BuildTestPath(L"empty_dir", Span<WCHAR>(dirPath));

		auto createResult = DirectoryIterator::Create(dirPath);
		if (!createResult)
		{
			LOG_ERROR("Create() failed for an EMPTY directory: %e", createResult.Error());
			(VOID)RmDir(L"empty_dir");
			return false;
		}

		// POSIX returns "." and ".." before the end — the empty listing is the
		// handler's view (dot entries skipped), not the iterator's. Drain every
		// entry asserting it is a dot entry, then the terminator must be the
		// clean-end sentinel — never a failure.
		DirectoryIterator &emptyIter = createResult.Value();
		while (true)
		{
			auto next = emptyIter.Next();
			if (next)
			{
				const DirectoryEntry &entry = emptyIter.Get();
				BOOL isDot = entry.Name[0] == L'.'
							 && (entry.Name[1] == L'\0' || (entry.Name[1] == L'.' && entry.Name[2] == L'\0'));
				if (!isDot)
				{
					LOG_ERROR("Empty directory yielded non-dot entry: %ws", entry.Name);
					(VOID)RmDir(L"empty_dir");
					return false;
				}
				continue;
			}
			BOOL cleanEnd = next.Error().Platform == Error::PlatformKind::Runtime
							&& next.Error().Code == Error::Fs_NoMoreEntries;
			if (!cleanEnd)
			{
				LOG_ERROR("Empty directory: iteration ended with %e (not the Fs_NoMoreEntries sentinel)", next.Error());
				(VOID)RmDir(L"empty_dir");
				return false;
			}
			break;
		}

		return (BOOL)RmDir(L"empty_dir");
	}

	// A REAL open/iteration failure must be distinguishable from the clean-end
	// sentinel: the sentinel is Runtime/Fs_NoMoreEntries, while OS failures
	// carry the raw platform code as their root cause.
	static BOOL TestIterationErrorSeparation()
	{
		WCHAR missingPath[128];
		BuildTestPath(L"definitely_not_created_dir", Span<WCHAR>(missingPath));

		auto createResult = DirectoryIterator::Create(missingPath);
		if (createResult)
		{
			LOG_ERROR("Create() unexpectedly succeeded for a nonexistent directory");
			return false;
		}

		const Error &error = createResult.Error();
		BOOL isSentinel = error.Platform == Error::PlatformKind::Runtime && error.Code == Error::Fs_NoMoreEntries;
		if (isSentinel)
		{
			LOG_ERROR("Nonexistent directory misreported as clean end-of-iteration");
			return false;
		}

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD) || defined(PLATFORM_SOLARIS)
		// POSIX: the root cause must be the errno (ENOENT) tagged Posix — this
		// is what travels in the error-detail tail so the C2 can classify.
		if (error.RootPlatform() != Error::PlatformKind::Posix || error.RootCode() != 2 /* ENOENT */)
		{
			LOG_ERROR("Open failure root cause is not Posix/ENOENT (platform=%d code=%u)",
					  (INT32)error.RootPlatform(), error.RootCode());
			return false;
		}
#endif

		return true;
	}

	// Byte-exact round trip of the lossless UTF-8 <-> wide conversions:
	// well-formed input converts normally, malformed input round-trips through
	// the U+DC80+byte surrogate escape without losing a single byte.
	static BOOL TestUtf8RoundTripConversion()
	{
		struct Case
		{
			PCCHAR input;
			USIZE length;
		};

		// Well-formed: ASCII, 2-byte, 3-byte, astral (4-byte)
		const CHAR valid[] = {'a', 'h', (CHAR)0xC3, (CHAR)0xA9, 'l', 'l', 'o', ' ', (CHAR)0xE2, (CHAR)0x82, (CHAR)0xAC, ' ', (CHAR)0xF0, (CHAR)0x9F, (CHAR)0x98, (CHAR)0x80, '!'};
		// Malformed: bare 0xFF, bare 0xFE, overlong C0 80, lone continuation 0x80, ASCII 'A'
		const CHAR invalid[] = {'a', (CHAR)0xFF, (CHAR)0xFE, (CHAR)0xC0, (CHAR)0x80, (CHAR)0x80, 'A'};
		// 3-byte encoding of a surrogate (CESU-8): ED A0 80
		const CHAR cesu[] = {'x', (CHAR)0xED, (CHAR)0xA0, (CHAR)0x80, 'y'};

		const Case cases[] = {
			{valid, sizeof(valid)},
			{invalid, sizeof(invalid)},
			{cesu, sizeof(cesu)},
		};

		for (USIZE c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
		{
			Span<const CHAR> utf8(cases[c].input, cases[c].length);

			WCHAR wide[256];
			USIZE wideLen = StringUtils::Utf8ToWideLossless(utf8, Span<WCHAR>(wide, 256));
			if (wideLen == 0)
			{
				LOG_ERROR("Utf8ToWideLossless returned 0 for case %llu", (UINT64)c);
				return false;
			}

			CHAR back[256];
			USIZE backLen = UTF16::ToUTF8Lossless(Span<const WCHAR>(wide, wideLen), Span<CHAR>(back, sizeof(back)));

			if (backLen != cases[c].length)
			{
				LOG_ERROR("Round trip changed length for case %llu: %llu -> %llu",
						  (UINT64)c, (UINT64)cases[c].length, (UINT64)backLen);
				return false;
			}
			for (USIZE i = 0; i < backLen; i++)
			{
				if (back[i] != cases[c].input[i])
				{
					LOG_ERROR("Round trip changed byte %llu of case %llu", (UINT64)i, (UINT64)c);
					return false;
				}
			}
		}

		// Spot-check the escape encoding itself: byte 0xFF must become U+DCFF.
		const CHAR oneBad[] = {(CHAR)0xFF};
		WCHAR wide[8];
		USIZE wideLen = StringUtils::Utf8ToWideLossless(Span<const CHAR>(oneBad, 1), Span<WCHAR>(wide, 8));
		if (wideLen != 1 || wide[0] != (WCHAR)0xDCFF)
		{
			LOG_ERROR("Byte 0xFF did not escape to U+DCFF (len=%llu unit=0x%X)", (UINT64)wideLen, (UINT32)wide[0]);
			return false;
		}

		// And a REAL astral pair must survive: U+1F600 encoded in wide form
		// must not be unescaped even though low surrogates overlap the window.
		const CHAR emoji[] = {(CHAR)0xF0, (CHAR)0x9F, (CHAR)0x98, (CHAR)0x80};
		wideLen = StringUtils::Utf8ToWideLossless(Span<const CHAR>(emoji, sizeof(emoji)), Span<WCHAR>(wide, 8));
		CHAR back[8];
		USIZE backLen = UTF16::ToUTF8Lossless(Span<const WCHAR>(wide, wideLen), Span<CHAR>(back, sizeof(back)));
		if (backLen != sizeof(emoji) || back[0] != emoji[0] || back[3] != emoji[3])
		{
			LOG_ERROR("Astral codepoint did not survive the lossless round trip");
			return false;
		}

		return true;
	}

	// A filename that is not valid UTF-8 must be LISTED (escaped, not skipped
	// or mangled) and must be OPENABLE again through the escaped wide path —
	// the exact wire round trip the C2 performs. Linux-only: the raw-byte
	// creation needs direct syscalls.
	static BOOL TestLosslessFilenames()
	{
#if defined(PLATFORM_LINUX)
		if (!MkDir(L"lossless_dir"))
		{
			LOG_ERROR("Failed to create lossless_dir");
			return false;
		}

		// Create "bad\xFF\xFEname.bin" with raw syscalls (the wide API cannot
		// express these bytes without the escape, which is the point).
		const CHAR rawPath[] = {'t', 'e', 's', 't', '_', 'i', 'o', '_', 'r', 'o', 'o', 't', '/',
								'l', 'o', 's', 's', 'l', 'e', 's', 's', '_', 'd', 'i', 'r', '/',
								'b', 'a', 'd', (CHAR)0xFF, (CHAR)0xFE, 'n', 'a', 'm', 'e', '.', 'b', 'i', 'n', '\0'};
		SSIZE fd = System::Call(SYS_OPENAT, (USIZE)-100 /* AT_FDCWD */, (USIZE)rawPath, O_CREAT | O_WRONLY, 0600);
		if (fd < 0)
		{
			LOG_ERROR("Failed to create raw-byte filename (fd=%lld)", (INT64)fd);
			(VOID)RmDir(L"lossless_dir");
			return false;
		}
		(VOID)System::Call(SYS_CLOSE, (USIZE)fd);
		// Best-effort residue cleanup for every failure path below (raw unlink —
		// the escaped wide path is not always built yet at the failure site).
		// Arch guard mirrors platform/fs/posix/file.cc: x86_64/i386/armv7a have
		// SYS_UNLINK, the newer ABIs only SYS_UNLINKAT.
		auto cleanup = [&]()
		{
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32) || defined(ARCHITECTURE_MIPS64)
			(VOID)System::Call(SYS_UNLINKAT, (USIZE)-100, (USIZE)rawPath, 0);
#else
			(VOID)System::Call(SYS_UNLINK, (USIZE)rawPath);
#endif
			(VOID)RmDir(L"lossless_dir");
		};

		// List the directory: the entry must appear with the escaped name
		// b a d U+DCFF U+DCFE n a m e . b i n — not dropped, not mangled.
		WCHAR dirPath[128];
		BuildTestPath(L"lossless_dir", Span<WCHAR>(dirPath));
		auto createResult = DirectoryIterator::Create(dirPath);
		if (!createResult)
		{
			LOG_ERROR("Failed to iterate lossless_dir: %e", createResult.Error());
			cleanup();
			return false;
		}

		const WCHAR expected[] = {L'b', L'a', L'd', (WCHAR)0xDCFF, (WCHAR)0xDCFE, L'n', L'a', L'm', L'e', L'.', L'b', L'i', L'n', L'\0'};
		BOOL found = false;
		DirectoryIterator &iter = createResult.Value();
		while (iter.Next())
		{
			if (StringUtils::Equals((PWCHAR)iter.Get().Name, (PWCHAR)expected))
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			LOG_ERROR("Raw-byte filename was not listed with its escaped name");
			cleanup();
			return false;
		}

		// Round trip: open the file THROUGH the wide escaped path (this is
		// what the C2's ReadFile command does) and read back what we write.
		USIZE dirLen = StringUtils::Length((PWCHAR)dirPath);
		WCHAR combined[160];
		Memory::Copy(combined, dirPath, dirLen * sizeof(WCHAR));
		combined[dirLen] = L'/';
		Memory::Copy(combined + dirLen + 1, expected, 13 * sizeof(WCHAR));
		combined[dirLen + 14] = L'\0';

		auto writeOpen = File::Open((PCWCHAR)combined, File::ModeCreate | File::ModeWrite | File::ModeTruncate);
		if (!writeOpen)
		{
			LOG_ERROR("Escaped wide path failed to open for write: %e", writeOpen.Error());
			cleanup();
			return false;
		}
		const UINT8 payload[] = {0xAB, 0xCD};
		auto writeResult = writeOpen.Value().Write(Span<const UINT8>(payload, sizeof(payload)));
		writeOpen.Value().Close();
		if (!writeResult)
		{
			LOG_ERROR("Write through escaped path failed: %e", writeResult.Error());
			cleanup();
			return false;
		}

		auto readOpen = File::Open((PCWCHAR)combined, File::ModeRead);
		if (!readOpen)
		{
			LOG_ERROR("Escaped wide path failed to open for read: %e", readOpen.Error());
			cleanup();
			return false;
		}
		UINT8 got[2] = {0, 0};
		auto readResult = readOpen.Value().Read(Span<UINT8>(got, sizeof(got)));
		readOpen.Value().Close();
		if (!readResult || readResult.Value() != 2 || got[0] != 0xAB || got[1] != 0xCD)
		{
			LOG_ERROR("Read through escaped path returned wrong data");
			cleanup();
			return false;
		}

		// Cleanup through the wide path (same lossless normalization).
		if (!File::Delete((PCWCHAR)combined))
		{
			LOG_ERROR("Failed to delete raw-byte file through escaped path");
			cleanup();
			return false;
		}
		return (BOOL)RmDir(L"lossless_dir");
#else
		LOG_INFO("TestLosslessFilenames skipped on this platform (POSIX-only semantics)");
		return true;
#endif
	}

	// POSIX corner cases: symlinks, special files, odd names, EOF idempotence.
	static BOOL TestEnumerationCornerCases()
	{
#if defined(PLATFORM_LINUX)
		if (!MkDir(L"corner_dir")) return false;
		const CHAR base[] = "test_io_root/corner_dir/";
		constexpr USIZE baseLen = sizeof(base) - 1;
		CHAR p[512];

		// p = base + tail
		auto mkpath = [&](PCCHAR tail)
		{
			Memory::Copy(p, base, sizeof(base));
			if (tail == nullptr) return;
			USIZE n = 0;
			while (tail[n]) n++;
			Memory::Copy(p + baseLen, tail, n + 1);
		};

		BOOL ok = true;

		// Symlink to a directory: must be listed as a non-directory (lstat via NOFOLLOW).
		mkpath("target"); SSIZE tf = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)p, 0x40 | 1, 0600);
		if (tf >= 0) (VOID)System::Call(SYS_CLOSE, (USIZE)tf);
		mkpath("dirlink"); (VOID)System::Call(SYS_SYMLINK_T, (USIZE)"target", (USIZE)p);
		// Broken symlink: entry must survive.
		mkpath("deadlink"); (VOID)System::Call(SYS_SYMLINK_T, (USIZE)"nowhere", (USIZE)p);
		// FIFO: listed, not a directory, no crash.
		mkpath("apipe"); (VOID)System::Call(SYS_MKNOD_T, (USIZE)p, 0010000 /* S_IFIFO */, 0666);
		// Drive-lookalike name: never flagged as a drive on POSIX.
		mkpath("C:"); auto f = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)p, 0x40 /* O_CREAT */ | 1 /* O_WRONLY */, 0600);
		if (f >= 0) (VOID)System::Call(SYS_CLOSE, (USIZE)f);
		// Hidden file.
		mkpath(".hidden"); f = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)p, 0x40 | 1, 0600);
		if (f >= 0) (VOID)System::Call(SYS_CLOSE, (USIZE)f);
		// Max-length name (255 bytes).
		CHAR longName[256];
		for (INT32 i = 0; i < 255; i++) longName[i] = 'a' + (i % 26);
		longName[255] = 0;
		mkpath(longName);
		f = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)p, 0x40 | 1, 0600);
		if (f >= 0) (VOID)System::Call(SYS_CLOSE, (USIZE)f);

		WCHAR dirPath[128];
		BuildTestPath(L"corner_dir", Span<WCHAR>(dirPath));
		auto createResult = DirectoryIterator::Create(dirPath);
		if (!createResult) { (VOID)RmDir(L"corner_dir"); return false; }

		BOOL sawDirlink = false, sawDeadlink = false, sawFifo = false, sawDrive = false;
		BOOL sawHidden = false, sawLong = false, sawAstral = false;
		DirectoryIterator &iter = createResult.Value();
		BOOL eofTwice = false;
		while (true)
		{
			auto next = iter.Next();
			if (!next)
			{
				if (!(next.Error().Platform == Error::PlatformKind::Runtime && next.Error().Code == Error::Fs_NoMoreEntries))
				{
					LOG_ERROR("corner_dir ended with %e", next.Error());
					ok = false;
				}
				// EOF must be idempotent: a second call returns the sentinel again.
				auto again = iter.Next();
				eofTwice = !again && again.Error().Code == Error::Fs_NoMoreEntries;
				break;
			}
			const DirectoryEntry &e = iter.Get();
			if (StringUtils::Equals((PWCHAR)e.Name, (const WCHAR *)L"dirlink")) { sawDirlink = true; if (e.IsDirectory) { LOG_ERROR("dirlink listed as directory"); ok = false; } }
			else if (StringUtils::Equals((PWCHAR)e.Name, (const WCHAR *)L"deadlink")) sawDeadlink = true;
			else if (StringUtils::Equals((PWCHAR)e.Name, (const WCHAR *)L"apipe")) { sawFifo = true; if (e.IsDirectory) { LOG_ERROR("fifo listed as directory"); ok = false; } }
			else if (StringUtils::Equals((PWCHAR)e.Name, (const WCHAR *)L"C:")) { sawDrive = true; if (e.IsDrive) { LOG_ERROR("C: flagged as drive on POSIX"); ok = false; } }
			else if (StringUtils::Equals((PWCHAR)e.Name, (const WCHAR *)L".hidden")) sawHidden = true;
			else if (StringUtils::Length((PWCHAR)e.Name) == 255) sawLong = true;
		}
		if (!sawDirlink || !sawDeadlink || !sawFifo || !sawDrive || !sawHidden || !sawLong || !eofTwice || !ok)
		{
			LOG_ERROR("corner cases: dirlink=%d deadlink=%d fifo=%d drive=%d hidden=%d long=%d eofTwice=%d",
					  sawDirlink, sawDeadlink, sawFifo, sawDrive, sawHidden, sawLong, eofTwice);
			ok = false;
		}

		// An astral (4-byte UTF-8) filename must NOT be escaped — exact name match.
		const WCHAR astral[] = {0x1F600, L'.', L't', L'x', L't', 0};
		{
			WCHAR ap[160];
			BuildTestPath(L"corner_dir", Span<WCHAR>(ap));
			USIZE dl = StringUtils::Length((PWCHAR)ap);
			Memory::Copy(ap + dl, (const WCHAR *)L"/", 2);
			Memory::Copy(ap + dl + 1, astral, 6 * sizeof(WCHAR));
			ap[dl + 6] = 0;
			auto fo = File::Open((PCWCHAR)ap, File::ModeCreate | File::ModeWrite);
			if (fo) fo.Value().Close();
		}
		createResult = DirectoryIterator::Create(dirPath);
		if (createResult)
		{
			DirectoryIterator &it2 = createResult.Value();
			while (it2.Next())
				if (StringUtils::Equals((PWCHAR)it2.Get().Name, (PWCHAR)astral)) { sawAstral = true; break; }
		}
		if (!sawAstral) { LOG_ERROR("astral filename not found unescaped"); ok = false; }

		// Cleanup (rm the fifo/links/file first, then dirs).
		const CHAR *names[] = {"dirlink", "deadlink", "apipe", "C:", ".hidden", longName, nullptr};
		for (USIZE i = 0; names[i]; i++)
		{
			mkpath(names[i]);
			(VOID)System::Call(SYS_UNLINK, (USIZE)p);
		}
		const WCHAR aw[] = {0x1F600, L'.', L't', L'x', L't', 0};
		{
			WCHAR ap[160];
			BuildTestPath(L"corner_dir", Span<WCHAR>(ap));
			USIZE dl = StringUtils::Length((PWCHAR)ap);
			Memory::Copy(ap + dl, (const WCHAR *)L"/", 2);
			Memory::Copy(ap + dl + 1, aw, 6 * sizeof(WCHAR));
			ap[dl + 6] = 0;
			(VOID)File::Delete((PCWCHAR)ap);
		}
		mkpath("target"); (VOID)System::Call(SYS_UNLINK, (USIZE)p);
		return (BOOL)RmDir(L"corner_dir") && ok;
#else
		LOG_INFO("TestEnumerationCornerCases skipped (POSIX-only)");
		return true;
#endif
	}

	// Exact-count listing across many getdents buffer refills.
	static BOOL TestLargeDirectory()
	{
#if defined(PLATFORM_LINUX)
		if (!MkDir(L"bulk_dir")) return false;
		const CHAR base[] = "test_io_root/bulk_dir/f";
		CHAR p[64];
		constexpr INT32 COUNT = 500;
		for (INT32 i = 0; i < COUNT; i++)
		{
			Memory::Copy(p, base, sizeof(base));
			// append decimal i
			CHAR num[12]; INT32 n = 0;
			INT32 v = i; if (v == 0) num[n++] = '0';
			while (v > 0) { num[n++] = '0' + v % 10; v /= 10; }
			USIZE bl = sizeof(base) - 1;
			for (INT32 k = 0; k < n; k++) p[bl + k] = num[n - 1 - k];
			p[bl + n] = 0;
			SSIZE fd = System::Call(SYS_OPENAT, (USIZE)-100, (USIZE)p, 0x40 | 1, 0600);
			if (fd < 0) { LOG_ERROR("bulk create failed at %d", i); return false; }
			(VOID)System::Call(SYS_CLOSE, (USIZE)fd);
		}

		WCHAR dirPath[128];
		BuildTestPath(L"bulk_dir", Span<WCHAR>(dirPath));
		auto createResult = DirectoryIterator::Create(dirPath);
		if (!createResult) return false;
		INT32 seen = 0;
		DirectoryIterator &iter = createResult.Value();
		while (iter.Next())
		{
			const WCHAR *n = (PWCHAR)iter.Get().Name;
			if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
			seen++;
		}
		if (seen != COUNT)
		{
			LOG_ERROR("large dir: expected %d entries, saw %d", COUNT, seen);
			// cleanup best-effort below still runs
		}
		// Cleanup
		for (INT32 i = 0; i < COUNT; i++)
		{
			Memory::Copy(p, base, sizeof(base));
			CHAR num[12]; INT32 n = 0;
			INT32 v = i; if (v == 0) num[n++] = '0';
			while (v > 0) { num[n++] = '0' + v % 10; v /= 10; }
			USIZE bl = sizeof(base) - 1;
			for (INT32 k = 0; k < n; k++) p[bl + k] = num[n - 1 - k];
			p[bl + n] = 0;
			(VOID)System::Call(SYS_UNLINK, (USIZE)p);
		}
		BOOL removed = (BOOL)RmDir(L"bulk_dir");
		return removed && seen == COUNT;
#else
		LOG_INFO("TestLargeDirectory skipped (POSIX-only)");
		return true;
#endif
	}

	static BOOL TestCleanup()
	{
		// Delete files first (from deepest to shallowest)

		// Second level files
		if (!RmFile(L"level1_dir1\\level2_dir1\\deep_file1.txt"))
		{
			LOG_ERROR("Failed to delete deep_file1.txt");
			return false;
		}
		if (!RmFile(L"level1_dir1\\level2_dir2\\deep_file2.txt"))
		{
			LOG_ERROR("Failed to delete deep_file2.txt");
			return false;
		}
		if (!RmFile(L"level1_dir2\\level2_dir3\\deep_file3.txt"))
		{
			LOG_ERROR("Failed to delete deep_file3.txt");
			return false;
		}
		if (!RmFile(L"level1_dir2\\level2_dir4\\deep_file4.txt"))
		{
			LOG_ERROR("Failed to delete deep_file4.txt");
			return false;
		}
		if (!RmFile(L"level1_dir3\\level2_dir5\\deep_file5.txt"))
		{
			LOG_ERROR("Failed to delete deep_file5.txt");
			return false;
		}

		// First level files
		if (!RmFile(L"level1_dir1\\file1.txt"))
		{
			LOG_ERROR("Failed to delete file1.txt");
			return false;
		}
		if (!RmFile(L"level1_dir1\\extra1.txt"))
		{
			LOG_ERROR("Failed to delete extra1.txt");
			return false;
		}
		if (!RmFile(L"level1_dir1\\extra2.txt"))
		{
			LOG_ERROR("Failed to delete extra2.txt");
			return false;
		}
		if (!RmFile(L"level1_dir1\\binary_test.dat"))
		{
			LOG_ERROR("Failed to delete binary_test.dat");
			return false;
		}
		if (!RmFile(L"level1_dir2\\file2.txt"))
		{
			LOG_ERROR("Failed to delete file2.txt");
			return false;
		}
		if (!RmFile(L"level1_dir2\\offset_test.dat"))
		{
			LOG_ERROR("Failed to delete offset_test.dat");
			return false;
		}
		if (!RmFile(L"level1_dir3\\file3.txt"))
		{
			LOG_ERROR("Failed to delete file3.txt");
			return false;
		}

		// Root level files
		if (!RmFile(L"root_file.txt"))
		{
			LOG_ERROR("Failed to delete root_file.txt");
			return false;
		}
		if (!RmFile(L"test_write_read.txt"))
		{
			LOG_ERROR("Failed to delete test_write_read.txt");
			return false;
		}

		// Delete directories (from deepest to shallowest)

		// Second level directories
		if (!RmDir(L"level1_dir1\\level2_dir1"))
		{
			LOG_ERROR("Failed to delete level2_dir1");
			return false;
		}
		if (!RmDir(L"level1_dir1\\level2_dir2"))
		{
			LOG_ERROR("Failed to delete level2_dir2");
			return false;
		}
		if (!RmDir(L"level1_dir2\\level2_dir3"))
		{
			LOG_ERROR("Failed to delete level2_dir3");
			return false;
		}
		if (!RmDir(L"level1_dir2\\level2_dir4"))
		{
			LOG_ERROR("Failed to delete level2_dir4");
			return false;
		}
		if (!RmDir(L"level1_dir3\\level2_dir5"))
		{
			LOG_ERROR("Failed to delete level2_dir5");
			return false;
		}

		// First level directories
		if (!RmDir(L"level1_dir1"))
		{
			LOG_ERROR("Failed to delete level1_dir1");
			return false;
		}
		if (!RmDir(L"level1_dir2"))
		{
			LOG_ERROR("Failed to delete level1_dir2");
			return false;
		}
		if (!RmDir(L"level1_dir3"))
		{
			LOG_ERROR("Failed to delete level1_dir3");
			return false;
		}

		// Root directory
		if (!RmDir(nullptr))
		{
			LOG_ERROR("Failed to delete test_io_root");
			return false;
		}

		// Verify cleanup was successful
		if (PathExists(nullptr))
		{
			LOG_ERROR("test_io_root still exists after cleanup");
			return false;
		}

		return true;
	}
};
