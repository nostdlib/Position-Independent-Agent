/**
 * portable_device_tests.h - Portable-device pseudo-root test suite
 *
 * Windows: exercises the ::mtp- root-listing shapes, navigation, and reads
 * against whatever devices are attached (all cases pass with skip-logs when
 * none is present). Linux: exercises the udisks/GVFS portable-root collectors
 * against a synthetic fixture tree and the empty-path iterator regression.
 */
#pragma once

#include "lib/runtime.h"
#include "tests.h"
#include "platform/fs/posix/portable_roots.h"

class PortableDeviceTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Portable Device Tests...");

		RunTest(allPassed, &TestRootListingShapes, "Root listing portable-device shapes");
		RunTest(allPassed, &TestPseudoRootNavigation, "Portable-device pseudo-root navigation");
		RunTest(allPassed, &TestPseudoRootRead, "Portable-device pseudo-root read");
		RunTest(allPassed, &TestPortableRootFixtures, "Portable-root collectors (fixture tree)");
		RunTest(allPassed, &TestEmptyPathStillEnumerates, "Empty-path iterator regression");

		if (allPassed)
			LOG_INFO("All PortableDevice tests passed!");
		else
			LOG_ERROR("Some PortableDevice tests failed!");

		return allPassed;
	}

private:
	static BOOL IsHexLower(WCHAR c)
	{
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
	}

	/// TRUE for `::mtp-<16 lowercase hex>`, optionally followed by `-<label>`.
	static BOOL IsDeviceRootName(const WCHAR *name)
	{
		const WCHAR prefix[] = L"::mtp-";
		for (USIZE i = 0; i < 6; i++)
		{
			if (name[i] != prefix[i])
				return false;
		}
		for (USIZE i = 6; i < 22; i++)
		{
			if (!IsHexLower(name[i]))
				return false;
		}
		return name[22] == L'\0' || name[22] == L'-';
	}

	/// Copies the first `::mtp-` root-listing entry into out; FALSE if none.
	static BOOL FindDevicePseudoRoot(WCHAR *out, USIZE cap)
	{
		auto rootResult = DirectoryIterator::Create(L"");
		if (!rootResult)
			return false;
		DirectoryIterator &rootIter = rootResult.Value();
		BOOL found = false;
		while (rootIter.Next())
		{
			const DirectoryEntry &entry = rootIter.Get();
			if (!entry.IsDrive || !IsDeviceRootName(entry.Name))
				continue;
			USIZE length = StringUtils::Length(entry.Name);
			if (length >= cap)
				continue;
			StringUtils::Copy(Span<WCHAR>(out, cap), Span<const WCHAR>(entry.Name, length));
			found = true;
			break;
		}
		rootIter.Close();
		return found;
	}

#if defined(PLATFORM_WINDOWS)

	/// Root listing: every entry is a real drive "X:\" or a well-formed
	/// ::mtp- pseudo-root; devices must be removable drives carrying a token.
	static BOOL TestRootListingShapes()
	{
		auto rootResult = DirectoryIterator::Create(L"");
		if (!rootResult)
		{
			LOG_ERROR("Failed to create DirectoryIterator for root");
			return false;
		}
		DirectoryIterator &rootIter = rootResult.Value();

		INT32 driveCount = 0;
		INT32 deviceCount = 0;
		while (rootIter.Next())
		{
			const DirectoryEntry &entry = rootIter.Get();
			if (!entry.IsDrive || !entry.IsDirectory)
			{
				LOG_ERROR("Root entry %ws is not a drive", entry.Name);
				return false;
			}
			if (IsDeviceRootName(entry.Name))
			{
				deviceCount++;
				if (entry.Type != 2) // DRIVE_REMOVABLE
				{
					LOG_ERROR("Device entry %ws Type=%u, expected 2", entry.Name, entry.Type);
					return false;
				}
				if (entry.VolumeSerial == 0)
				{
					LOG_ERROR("Device entry %ws carries no token", entry.Name);
					return false;
				}
			}
			else if (entry.Name[1] == L':' && entry.Name[2] == L'\\' && entry.Name[3] == L'\0')
			{
				driveCount++;
			}
			else
			{
				LOG_ERROR("Root entry %ws matches neither drive nor pseudo-root grammar", entry.Name);
				return false;
			}
		}
		rootIter.Close();

		if (driveCount == 0)
		{
			LOG_ERROR("Root listing lost the real drives");
			return false;
		}
		if (deviceCount > 0)
			LOG_INFO("  Root listing: %d drives, %d portable device(s)", driveCount, deviceCount);
		else
			LOG_INFO("  Root listing: %d drives, no portable devices attached", driveCount);
		return true;
	}

	/// Navigation inside a device pseudo-root; missing segments must fail
	/// cleanly. Skips (pass) when no device is attached.
	static BOOL TestPseudoRootNavigation()
	{
		WCHAR pseudoRoot[256];
		if (!FindDevicePseudoRoot(pseudoRoot, 256))
		{
			LOG_INFO("  Navigation skipped (no portable device attached)");
			return true;
		}

		WCHAR navPath[512];
		USIZE rootLength = StringUtils::Length(pseudoRoot);
		StringUtils::Copy(Span<WCHAR>(navPath, 512), Span<const WCHAR>(pseudoRoot, rootLength));
		const WCHAR missing[] = L"\\definitely_missing_segment_xyz";
		StringUtils::Copy(Span<WCHAR>(navPath + rootLength, 512 - rootLength), Span<const WCHAR>(missing, sizeof(missing) / sizeof(missing[0]) - 1));

		auto missingResult = DirectoryIterator::Create(navPath);
		if (missingResult)
		{
			missingResult.Value().Close();
			LOG_ERROR("Navigation into a missing device segment unexpectedly succeeded");
			return false;
		}

		auto deviceResult = DirectoryIterator::Create(pseudoRoot);
		if (!deviceResult)
		{
			LOG_ERROR("Create() failed for an attached device pseudo-root: %e", deviceResult.Error());
			return false;
		}
		DirectoryIterator &deviceIter = deviceResult.Value();
		INT32 entryCount = 0;
		while (deviceIter.Next())
		{
			const DirectoryEntry &entry = deviceIter.Get();
			if (StringUtils::Length(entry.Name) == 0)
			{
				LOG_ERROR("Device pseudo-root yielded an empty entry name");
				deviceIter.Close();
				return false;
			}
			entryCount++;
		}
		deviceIter.Close();
		LOG_INFO("  Navigation: device root listed %d entr(ies)", entryCount);
		return true;
	}

	/// Reads the first file found one or two levels under a device root.
	/// Skips (pass) when no device or no file is reachable.
	static BOOL TestPseudoRootRead()
	{
		WCHAR pseudoRoot[256];
		if (!FindDevicePseudoRoot(pseudoRoot, 256))
		{
			LOG_INFO("  Read skipped (no portable device attached)");
			return true;
		}

		WCHAR filePath[512];
		BOOL found = false;

		auto deviceResult = DirectoryIterator::Create(pseudoRoot);
		if (!deviceResult)
		{
			LOG_ERROR("Create() failed for an attached device pseudo-root");
			return false;
		}
		DirectoryIterator &deviceIter = deviceResult.Value();
		while (deviceIter.Next() && !found)
		{
			const DirectoryEntry &child = deviceIter.Get();
			USIZE rootLength = StringUtils::Length(pseudoRoot);
			StringUtils::Copy(Span<WCHAR>(filePath, 512), Span<const WCHAR>(pseudoRoot, rootLength));
			filePath[rootLength] = L'\\';
			StringUtils::Copy(Span<WCHAR>(filePath + rootLength + 1, 512 - rootLength - 1), Span<const WCHAR>(child.Name, StringUtils::Length(child.Name)));

			if (!child.IsDirectory)
			{
				found = true;
				break;
			}

			auto subResult = DirectoryIterator::Create(filePath);
			if (!subResult)
				continue;
			DirectoryIterator &subIter = subResult.Value();
			while (subIter.Next())
			{
				const DirectoryEntry &grandChild = subIter.Get();
				if (grandChild.IsDirectory)
					continue;
				USIZE soFar = StringUtils::Length(filePath);
				filePath[soFar] = L'\\';
				StringUtils::Copy(Span<WCHAR>(filePath + soFar + 1, 512 - soFar - 1), Span<const WCHAR>(grandChild.Name, StringUtils::Length(grandChild.Name)));
				found = true;
				break;
			}
			subIter.Close();
		}
		deviceIter.Close();

		if (!found)
		{
			LOG_INFO("  Read skipped (device exposes no file in the first two levels)");
			return true;
		}

		auto fileResult = File::Open(filePath, File::ModeRead);
		if (!fileResult)
		{
			LOG_ERROR("File::Open failed on device object %ws: %e", filePath, fileResult.Error());
			return false;
		}
		File &file = fileResult.Value();
		auto seekResult = file.SetOffset(0);
		if (!seekResult)
		{
			LOG_ERROR("SetOffset(0) failed on device object %ws", filePath);
			file.Close();
			return false;
		}
		UINT8 buffer[1024];
		auto readResult = file.Read(Span<UINT8>(buffer, sizeof(buffer)));
		if (!readResult)
		{
			LOG_ERROR("Read failed on device object %ws: %e", filePath, readResult.Error());
			file.Close();
			return false;
		}
		LOG_INFO("  Read: %ws -> %u byte(s)", filePath, readResult.Value());
		file.Close();
		return true;
	}

	static BOOL TestPortableRootFixtures()
	{
		LOG_INFO("  Fixture collectors skipped (Linux only)");
		return true;
	}

	static BOOL TestEmptyPathStillEnumerates()
	{
		LOG_INFO("  Empty-path regression skipped (Linux collector wiring only)");
		return true;
	}

#elif defined(PLATFORM_LINUX)

	static BOOL TestRootListingShapes()
	{
		LOG_INFO("  Root shapes skipped (Windows pseudo-roots only)");
		return true;
	}

	static BOOL TestPseudoRootNavigation()
	{
		LOG_INFO("  Navigation skipped (Windows pseudo-roots only)");
		return true;
	}

	static BOOL TestPseudoRootRead()
	{
		LOG_INFO("  Read skipped (Windows pseudo-roots only)");
		return true;
	}

	// ── Fixture helpers (test_io_root portable-device tree) ─────────────

	static BOOL MakeDirChain(const WCHAR *path)
	{
		auto createResult = Directory::Create(path);
		return createResult || (BOOL)File::Exists(path);
	}

	static VOID RemoveDir(const WCHAR *path)
	{
		(VOID)Directory::Delete(path);
	}

	/// TRUE when `out` contains an entry with exactly this name.
	static BOOL ContainsEntry(const Vector<DirectoryEntry> &out, const WCHAR *expected)
	{
		for (INT32 i = 0; i < out.Count; i++)
		{
			if (StringUtils::Compare(Span<const WCHAR>(out.Data[i].Name, StringUtils::Length(out.Data[i].Name)), Span<const WCHAR>(expected, StringUtils::Length(expected))))
				return true;
		}
		return false;
	}

	/// Validates the drive-entry shape shared by both collectors.
	static BOOL AssertDriveEntry(const DirectoryEntry &entry, PCCHAR context)
	{
		if (!entry.IsDrive || !entry.IsDirectory || entry.Type != 2)
		{
			LOG_ERROR("%s: entry %ws has wrong drive shape", context, entry.Name);
			return false;
		}
		USIZE length = StringUtils::Length(entry.Name);
		if (length == 0 || entry.Name[length - 1] != L'/')
		{
			LOG_ERROR("%s: entry %ws lacks the trailing '/'", context, entry.Name);
			return false;
		}
		if (entry.VolumeSerial != 0)
		{
			LOG_ERROR("%s: entry %ws carries a volume serial", context, entry.Name);
			return false;
		}
		return true;
	}

	/// CollectMountedMedia + CollectGvfsMounts against a synthetic tree.
	static BOOL TestPortableRootFixtures()
	{
		Vector<DirectoryEntry> mediaMounts;
		Vector<DirectoryEntry> gvfsMounts;
		if (!mediaMounts.Init() || !gvfsMounts.Init())
			return false;

		// Fixture tree: media/alice/{USB1,USB2} + run/user/1000/gvfs/{mtp:*,gphoto2:*,other}
		const WCHAR *dirsToCreate[] = {
			L"test_io_root",
			L"test_io_root/media",
			L"test_io_root/media/alice",
			L"test_io_root/media/alice/USB1",
			L"test_io_root/media/alice/USB2",
			L"test_io_root/run",
			L"test_io_root/run/user",
			L"test_io_root/run/user/1000",
			L"test_io_root/run/user/1000/gvfs",
			L"test_io_root/run/user/1000/gvfs/mtp:host=Fake",
			L"test_io_root/run/user/1000/gvfs/gphoto2:host=Cam",
			L"test_io_root/run/user/1000/gvfs/other",
		};
		for (USIZE i = 0; i < sizeof(dirsToCreate) / sizeof(dirsToCreate[0]); i++)
		{
			if (!MakeDirChain(dirsToCreate[i]))
			{
				LOG_ERROR("Failed to create fixture dir %ws", dirsToCreate[i]);
				return false;
			}
		}

		BOOL allOk = true;

		PortableRoots::CollectMountedMedia("test_io_root/media", mediaMounts);
		if (mediaMounts.Count != 2 || !ContainsEntry(mediaMounts, L"test_io_root/media/alice/USB1/") || !ContainsEntry(mediaMounts, L"test_io_root/media/alice/USB2/"))
		{
			LOG_ERROR("CollectMountedMedia returned %d entries, expected exactly USB1+USB2", mediaMounts.Count);
			allOk = false;
		}
		for (INT32 i = 0; allOk && i < mediaMounts.Count; i++)
			allOk = AssertDriveEntry(mediaMounts.Data[i], "CollectMountedMedia");

		PortableRoots::CollectGvfsMounts("test_io_root/run/user", gvfsMounts);
		if (gvfsMounts.Count != 2 || !ContainsEntry(gvfsMounts, L"test_io_root/run/user/1000/gvfs/mtp:host=Fake/") || !ContainsEntry(gvfsMounts, L"test_io_root/run/user/1000/gvfs/gphoto2:host=Cam/"))
		{
			LOG_ERROR("CollectGvfsMounts returned %d entries, expected exactly mtp+gphoto2 (other excluded)", gvfsMounts.Count);
			allOk = false;
		}
		for (INT32 i = 0; allOk && i < gvfsMounts.Count; i++)
			allOk = AssertDriveEntry(gvfsMounts.Data[i], "CollectGvfsMounts");

		// Cleanup deepest-first; failures tolerated (best-effort teardown).
		for (INT32 i = (INT32)(sizeof(dirsToCreate) / sizeof(dirsToCreate[0])) - 1; i >= 0; i--)
			RemoveDir(dirsToCreate[i]);

		return allOk;
	}

	/// The empty-path root iterator must keep succeeding with the collector
	/// wired in (regression: collectors must not break Create). Whenever this
	/// host has portable mounts, each must appear in the listing — catches the
	/// collector feeding a vector that was never Init()'ed (Add then silently
	/// drops every entry).
	static BOOL TestEmptyPathStillEnumerates()
	{
		Vector<DirectoryEntry> mounts;
		if (!mounts.Init())
			return false;
		PortableRoots::CollectPortableRoots(mounts);

		auto rootResult = DirectoryIterator::Create(L"");
		if (!rootResult)
		{
			LOG_ERROR("Empty-path Create() failed after portable-root wiring: %e", rootResult.Error());
			return false;
		}
		DirectoryIterator &rootIter = rootResult.Value();
		Vector<DirectoryEntry> listing;
		if (!listing.Init())
		{
			rootIter.Close();
			return false;
		}
		INT32 entryCount = 0;
		while (rootIter.Next())
		{
			if (!listing.Add(rootIter.Get()))
			{
				LOG_ERROR("Empty-path listing vector ran out of memory");
				rootIter.Close();
				return false;
			}
			entryCount++;
		}
		rootIter.Close();

		BOOL allOk = true;
		for (INT32 i = 0; allOk && i < mounts.Count; i++)
		{
			if (!ContainsEntry(listing, mounts.Data[i].Name))
			{
				LOG_ERROR("Portable mount %ws missing from the empty-path listing", mounts.Data[i].Name);
				allOk = false;
			}
		}
		LOG_INFO("  Empty-path root iteration: %d entr(ies), %d portable mount(s)", entryCount, mounts.Count);
		return allOk;
	}

#else // Other POSIX platforms: Linux collectors and Windows pseudo-roots are out of scope

	static BOOL TestRootListingShapes() { return true; }
	static BOOL TestPseudoRootNavigation() { return true; }
	static BOOL TestPseudoRootRead() { return true; }
	static BOOL TestPortableRootFixtures() { return true; }

	static BOOL TestEmptyPathStillEnumerates()
	{
		LOG_INFO("  Portable-device roots unsupported on this platform (skipped)");
		return true;
	}

#endif
};
