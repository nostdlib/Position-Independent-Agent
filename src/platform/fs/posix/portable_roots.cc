#include "platform/fs/posix/portable_roots.h"
#include "platform/fs/directory_iterator.h"
#include "core/string/string.h"
#include "core/memory/memory.h"

#if defined(PLATFORM_LINUX)

/// Win32 DRIVE_REMOVABLE — the drive-type vocabulary is platform-independent
/// on the wire (DirectoryEntry.Type), so the constant value is used directly.
constexpr UINT32 PORTABLE_DRIVE_TYPE_REMOVABLE = 2;

/// Maximum WCHARs of a mount entry name (DirectoryEntry.Name holds 255 + NUL).
constexpr USIZE PORTABLE_MAX_NAME = 255;

/// TRUE for the "." / ".." iterator entries.
static BOOL IsDotEntry(const DirectoryEntry &entry)
{
	return entry.Name[0] == L'.' && (entry.Name[1] == L'\0' || (entry.Name[1] == L'.' && entry.Name[2] == L'\0'));
}

/// TRUE when every character of the (non-empty) name is an ASCII digit.
static BOOL IsNumericName(const DirectoryEntry &entry)
{
	USIZE length = StringUtils::Length(entry.Name);
	if (length == 0)
		return false;
	for (USIZE i = 0; i < length; i++)
	{
		if (entry.Name[i] < L'0' || entry.Name[i] > L'9')
			return false;
	}
	return true;
}

/// Appends `<parent>/<child>/` to out as a drive-shaped entry.
static VOID AppendMountEntry(Vector<DirectoryEntry> &out, const WCHAR *parent, const WCHAR *child)
{
	WCHAR path[1024];
	USIZE parentLength = StringUtils::Length(parent);
	USIZE childLength = StringUtils::Length(child);
	if (parentLength + childLength + 3 > sizeof(path) / sizeof(path[0]))
		return; // path would not fit the build buffer

	Memory::Copy(path, parent, parentLength * sizeof(WCHAR));
	path[parentLength] = L'/';
	Memory::Copy(path + parentLength + 1, child, childLength * sizeof(WCHAR));
	path[parentLength + 1 + childLength] = L'/';
	path[parentLength + 2 + childLength] = L'\0';

	DirectoryEntry entry{};
	USIZE nameLength = parentLength + childLength + 2;
	if (nameLength > PORTABLE_MAX_NAME)
		return;
	Memory::Copy(entry.Name, path, (nameLength + 1) * sizeof(WCHAR));
	entry.IsDirectory = true;
	entry.IsDrive = true;
	entry.Type = PORTABLE_DRIVE_TYPE_REMOVABLE;
	(VOID)out.Add(entry);
}

/// Iterates dirPath's direct children, invoking the callback per real dir.
template <typename TCallback>
static VOID ForEachChildDir(const WCHAR *dirPath, TCallback callback)
{
	auto iteratorResult = DirectoryIterator::Create(dirPath);
	if (!iteratorResult)
		return; // EACCES / ENOENT: this subtree contributes nothing
	DirectoryIterator &iterator = iteratorResult.Value();
	while (iterator.Next())
	{
		const DirectoryEntry &entry = iterator.Get();
		if (IsDotEntry(entry) || !entry.IsDirectory)
			continue;
		callback(entry);
	}
	iterator.Close();
}

VOID PortableRoots::CollectMountedMedia(const CHAR *mediaDir, Vector<DirectoryEntry> &out)
{
	if (mediaDir == nullptr)
		return;
	WCHAR wideMediaDir[512];
	USIZE length = StringUtils::Utf8ToWide(Span<const CHAR>(mediaDir, StringUtils::Length(mediaDir)), Span<WCHAR>(wideMediaDir));
	if (length == 0)
		return;

	ForEachChildDir(wideMediaDir, [&](const DirectoryEntry &user)
	{
		WCHAR userPath[512];
		USIZE userLength = StringUtils::Length(user.Name);
		if (length + userLength + 2 > sizeof(userPath) / sizeof(userPath[0]))
			return;
		Memory::Copy(userPath, wideMediaDir, length * sizeof(WCHAR));
		userPath[length] = L'/';
		Memory::Copy(userPath + length + 1, user.Name, (userLength + 1) * sizeof(WCHAR));

		ForEachChildDir(userPath, [&](const DirectoryEntry &label)
		{
			AppendMountEntry(out, userPath, label.Name);
		});
	});
}

VOID PortableRoots::CollectGvfsMounts(const CHAR *runUserDir, Vector<DirectoryEntry> &out)
{
	if (runUserDir == nullptr)
		return;
	WCHAR wideRunUser[512];
	USIZE length = StringUtils::Utf8ToWide(Span<const CHAR>(runUserDir, StringUtils::Length(runUserDir)), Span<WCHAR>(wideRunUser));
	if (length == 0)
		return;

	ForEachChildDir(wideRunUser, [&](const DirectoryEntry &uid)
	{
		if (!IsNumericName(uid))
			return; // /run/user only carries numeric uids
		WCHAR gvfsPath[512];
		USIZE uidLength = StringUtils::Length(uid.Name);
		if (length + uidLength + 7 > sizeof(gvfsPath) / sizeof(gvfsPath[0]))
			return;
		Memory::Copy(gvfsPath, wideRunUser, length * sizeof(WCHAR));
		gvfsPath[length] = L'/';
		Memory::Copy(gvfsPath + length + 1, uid.Name, uidLength * sizeof(WCHAR));
		gvfsPath[length + 1 + uidLength] = L'/';
		const WCHAR gvfs[] = L"gvfs";
		Memory::Copy(gvfsPath + length + 2 + uidLength, gvfs, sizeof(gvfs));

		ForEachChildDir(gvfsPath, [&](const DirectoryEntry &mount)
		{
			USIZE nameLength = StringUtils::Length(mount.Name);
			BOOL isDeviceMount = (nameLength >= 4 && StringUtils::Compare(Span<const WCHAR>(mount.Name, 4), Span<const WCHAR>(L"mtp:", 4), true)) ||
								 (nameLength >= 8 && StringUtils::Compare(Span<const WCHAR>(mount.Name, 8), Span<const WCHAR>(L"gphoto2:", 8), true));
			if (isDeviceMount)
				AppendMountEntry(out, gvfsPath, mount.Name);
		});
	});
}

VOID PortableRoots::CollectPortableRoots(Vector<DirectoryEntry> &out)
{
	CollectMountedMedia("/media", out);
	CollectMountedMedia("/run/media", out);
	CollectGvfsMounts("/run/user", out);
}

#else // !PLATFORM_LINUX — portable-device roots are Linux-only

VOID PortableRoots::CollectPortableRoots(Vector<DirectoryEntry> &out)
{
	(void)out;
}

VOID PortableRoots::CollectMountedMedia(const CHAR *mediaDir, Vector<DirectoryEntry> &out)
{
	(void)mediaDir;
	(void)out;
}

VOID PortableRoots::CollectGvfsMounts(const CHAR *runUserDir, Vector<DirectoryEntry> &out)
{
	(void)runUserDir;
	(void)out;
}

#endif
