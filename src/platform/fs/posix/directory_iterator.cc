#include "platform/fs/directory_iterator.h"
#include "platform/fs/posix/posix_path.h"
#include "core/memory/memory.h"
#if defined(PLATFORM_LINUX)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#elif defined(PLATFORM_ANDROID)
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#elif defined(PLATFORM_MACOS)
#include "platform/kernel/macos/syscall.h"
#include "platform/kernel/macos/system.h"
#elif defined(PLATFORM_IOS)
#include "platform/kernel/ios/syscall.h"
#include "platform/kernel/ios/system.h"
#elif defined(PLATFORM_SOLARIS)
#include "platform/kernel/solaris/syscall.h"
#include "platform/kernel/solaris/system.h"
#elif defined(PLATFORM_FREEBSD)
#include "platform/kernel/freebsd/syscall.h"
#include "platform/kernel/freebsd/system.h"
#endif
#include "core/string/string.h"

// =============================================================================
// DirectoryIterator Implementation
// =============================================================================

DirectoryIterator::DirectoryIterator()
	: handle((PVOID)INVALID_FD), currentEntry{}, isFirst(false), bytesRead(0), bufferPosition(0)
{
	Memory::Zero(buffer, sizeof(buffer));
}

Result<DirectoryIterator, Error> DirectoryIterator::Create(PCWCHAR path)
{
	DirectoryIterator iter;
	CHAR utf8Path[1024];

	if (path && path[0] != L'\0')
	{
		NormalizePathToUtf8(path, Span<CHAR>(utf8Path));
	}
	else
	{
		utf8Path[0] = '/';
		utf8Path[1] = '\0';
	}

	SSIZE fd;
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))
	// RISC-V: omit O_DIRECTORY — QEMU user-mode does not translate the
	// asm-generic O_DIRECTORY (0x4000) to the host value, so the flag is
	// mis-interpreted as O_DIRECT on x86_64 hosts.  Safety is preserved
	// because getdents64 returns ENOTDIR on non-directory fds.
	INT32 openFlags = O_RDONLY;
#if defined(ARCHITECTURE_AARCH64)
	openFlags |= O_DIRECTORY;
#endif
	fd = System::Call(SYS_OPENAT, AT_FDCWD, (USIZE)utf8Path, openFlags, 0);
#elif defined(PLATFORM_SOLARIS)
	fd = System::Call(SYS_OPENAT, AT_FDCWD, (USIZE)utf8Path, (USIZE)(O_RDONLY | O_DIRECTORY), (USIZE)0);
#else
	fd = System::Call(SYS_OPEN, (USIZE)utf8Path, O_RDONLY | O_DIRECTORY);
#endif

	if (fd < 0)
	{
		return Result<DirectoryIterator, Error>::Err(Error::Posix((UINT32)(-fd)), Error::Fs_OpenFailed);
	}

	iter.handle = (PVOID)fd;
	iter.isFirst = true;
	return Result<DirectoryIterator, Error>::Ok(static_cast<DirectoryIterator &&>(iter));
}

DirectoryIterator::DirectoryIterator(DirectoryIterator &&other) noexcept
	: handle(other.handle), currentEntry(other.currentEntry), isFirst(other.isFirst), bytesRead(other.bytesRead), bufferPosition(other.bufferPosition)
{
	Memory::Copy(buffer, other.buffer, sizeof(buffer));
	other.handle = (PVOID)INVALID_FD;
}

DirectoryIterator &DirectoryIterator::operator=(DirectoryIterator &&other) noexcept
{
	if (this != &other)
	{
		if (IsValid())
			Close();
		handle = other.handle;
		currentEntry = other.currentEntry;
		isFirst = other.isFirst;
		bytesRead = other.bytesRead;
		bufferPosition = other.bufferPosition;
		Memory::Copy(buffer, other.buffer, sizeof(buffer));
		other.handle = (PVOID)INVALID_FD;
	}
	return *this;
}

VOID DirectoryIterator::Close()
{
	if (IsValid())
	{
		System::Call(SYS_CLOSE, (USIZE)handle);
		handle = (PVOID)INVALID_FD;
	}
}

Result<VOID, Error> DirectoryIterator::Next()
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_ReadFailed);

	if (isFirst || bufferPosition >= bytesRead)
	{
		isFirst = false;
#if defined(PLATFORM_SOLARIS) && (defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_AARCH64))
		// LP64 Solaris: getdents64 triggers SIGSYS for 64-bit processes.
		// Use getdents (81) which natively returns 64-bit dirent on LP64.
		bytesRead = (INT32)System::Call(SYS_GETDENTS, (USIZE)handle, (USIZE)buffer, sizeof(buffer));
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_SOLARIS)
		bytesRead = (INT32)System::Call(SYS_GETDENTS64, (USIZE)handle, (USIZE)buffer, sizeof(buffer));
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
		USIZE basep = 0;
		bytesRead = (INT32)System::Call(SYS_GETDIRENTRIES64, (USIZE)handle, (USIZE)buffer, sizeof(buffer), (USIZE)&basep);
#elif defined(PLATFORM_FREEBSD)
		USIZE basep = 0;
		bytesRead = (INT32)System::Call(SYS_GETDIRENTRIES, (USIZE)handle, (USIZE)buffer, sizeof(buffer), (USIZE)&basep);
#endif

		if (bytesRead < 0)
			return Result<VOID, Error>::Err(Error::Posix((UINT32)(-bytesRead)), Error::Fs_ReadFailed);
		if (bytesRead == 0)
			return Result<VOID, Error>::Err(Error::Fs_ReadFailed);
		bufferPosition = 0;
	}

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	LinuxDirent64 *d = (LinuxDirent64 *)(buffer + bufferPosition);
#elif defined(PLATFORM_SOLARIS)
	SolarisDirent64 *d = (SolarisDirent64 *)(buffer + bufferPosition);
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
	BsdDirent64 *d = (BsdDirent64 *)(buffer + bufferPosition);
#elif defined(PLATFORM_FREEBSD)
	FreeBsdDirent *d = (FreeBsdDirent *)(buffer + bufferPosition);
#endif

	StringUtils::Utf8ToWide(Span<const CHAR>(d->Name, StringUtils::Length(d->Name)), Span<WCHAR>(currentEntry.Name, 256));

	// --- Populate metadata via fstatat (Linux/Android/Solaris) ---
	// getdents does not return size/timestamps; fstatat fills them in.
	// Also provides reliable IsDirectory (d_type can be DT_UNKNOWN on some filesystems).
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_SOLARIS)
	{
		UINT8 statbuf[256];
		Memory::Zero(statbuf, sizeof(statbuf));

#if defined(ARCHITECTURE_X86_64) && !defined(PLATFORM_SOLARIS)
		SSIZE statResult = System::Call(SYS_NEWFSTATAT, (USIZE)handle, (USIZE)d->Name, (USIZE)statbuf, 0);
#elif (defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A)) && !defined(PLATFORM_SOLARIS)
		SSIZE statResult = System::Call(SYS_FSTATAT64, (USIZE)handle, (USIZE)d->Name, (USIZE)statbuf, 0);
#else // aarch64, riscv64, riscv32, mips64, Solaris
		SSIZE statResult = System::Call(SYS_FSTATAT, (USIZE)handle, (USIZE)d->Name, (USIZE)statbuf, 0);
#endif

		if (statResult == 0)
		{
			// Architecture-specific stat field offsets: st_mode, st_size, st_mtime
#if defined(PLATFORM_SOLARIS) && defined(ARCHITECTURE_I386)
			constexpr USIZE OFF_MODE = 20;
			constexpr USIZE OFF_SIZE = 44;
			constexpr USIZE OFF_MTIME = 72;
			constexpr BOOL MTIME_64 = false;
#elif defined(PLATFORM_SOLARIS)
			constexpr USIZE OFF_MODE = 16;
			constexpr USIZE OFF_SIZE = 48;
			constexpr USIZE OFF_MTIME = 88;
			constexpr BOOL MTIME_64 = true;
#elif defined(ARCHITECTURE_X86_64)
			// x86_64 struct stat: st_nlink is 8 bytes, pushing st_mode to 24
			constexpr USIZE OFF_MODE = 24;
			constexpr USIZE OFF_SIZE = 48;
			constexpr USIZE OFF_MTIME = 88;
			constexpr BOOL MTIME_64 = true;
#elif defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A)
			// i386/armv7a struct stat64
			constexpr USIZE OFF_MODE = 16;
			constexpr USIZE OFF_SIZE = 44;
			constexpr USIZE OFF_MTIME = 72;
			constexpr BOOL MTIME_64 = false;
#elif defined(ARCHITECTURE_MIPS64)
			// MIPS64 n64 struct stat
			constexpr USIZE OFF_MODE = 24;
			constexpr USIZE OFF_SIZE = 56;
			constexpr USIZE OFF_MTIME = 72;
			constexpr BOOL MTIME_64 = false;
#else // aarch64, riscv64, riscv32 — generic Linux stat
			constexpr USIZE OFF_MODE = 16;
			constexpr USIZE OFF_SIZE = 48;
			constexpr USIZE OFF_MTIME = 88;
			constexpr BOOL MTIME_64 = true;
#endif

			UINT32 mode = *(UINT32 *)(statbuf + OFF_MODE);
			currentEntry.IsDirectory = ((mode & 0xF000) == 0x4000); // S_IFDIR

			INT64 fileSize = *(INT64 *)(statbuf + OFF_SIZE);
			currentEntry.Size = (fileSize > 0) ? (UINT64)fileSize : 0;

			INT64 mtime;
			if constexpr (MTIME_64)
				mtime = *(INT64 *)(statbuf + OFF_MTIME);
			else
				mtime = (INT64)(*(INT32 *)(statbuf + OFF_MTIME));
			currentEntry.LastModifiedTime = (UINT64)mtime;
			currentEntry.CreationTime = currentEntry.LastModifiedTime;
		}
		else
		{
			currentEntry.Size = 0;
			currentEntry.CreationTime = 0;
			currentEntry.LastModifiedTime = 0;
#if defined(PLATFORM_SOLARIS)
			currentEntry.IsDirectory = false;
#else
			currentEntry.IsDirectory = (d->Type == DT_DIR);
#endif
		}
	}
#if defined(PLATFORM_SOLARIS)
	currentEntry.Type = currentEntry.IsDirectory ? 4 : 0;
#else
	currentEntry.Type = (UINT32)d->Type;
#endif
#else // macOS, iOS, FreeBSD — fstatat for size/timestamps, dirent d_type as fallback
	{
		UINT8 statbuf[256];
		Memory::Zero(statbuf, sizeof(statbuf));

#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
		SSIZE statResult = System::Call(SYS_FSTATAT64, (USIZE)handle, (USIZE)d->Name, (USIZE)statbuf, 0);
#elif defined(PLATFORM_FREEBSD)
		SSIZE statResult = System::Call(SYS_FSTATAT, (USIZE)handle, (USIZE)d->Name, (USIZE)statbuf, 0);
#endif

		if (statResult == 0)
		{
			// Architecture-specific stat field offsets: st_mode, st_size, st_mtime
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
			// macOS/iOS stat64: dev(4) mode(2) nlink(2) ino(8) uid(4) gid(4) rdev(4) pad(4) atim(16) mtim(16) ctim(16) birthtim(16) size(8)
			constexpr USIZE OFF_MODE = 4;
			constexpr USIZE OFF_SIZE = 96;
			constexpr USIZE OFF_MTIME = 48;
			constexpr BOOL MTIME_64 = true;
#elif defined(PLATFORM_FREEBSD) && defined(ARCHITECTURE_I386)
			// FreeBSD 12+ i386 stat: dev(8) ino(8) nlink(8) mode(2) pad(2) uid(4) gid(4) pad(4) rdev(8) atim(12) mtim(12) ctim(12) birthtim(12) size(8)
			// timespec is 12 bytes on i386 (int64_t tv_sec + int32_t tv_nsec)
			constexpr USIZE OFF_MODE = 24;
			constexpr USIZE OFF_SIZE = 96;
			constexpr USIZE OFF_MTIME = 60;
			constexpr BOOL MTIME_64 = true;
#elif defined(PLATFORM_FREEBSD)
			// FreeBSD 12+ LP64 stat: dev(8) ino(8) nlink(8) mode(2) pad(2) uid(4) gid(4) pad(4) rdev(8) atim(16) mtim(16) ctim(16) birthtim(16) size(8)
			constexpr USIZE OFF_MODE = 24;
			constexpr USIZE OFF_SIZE = 112;
			constexpr USIZE OFF_MTIME = 64;
			constexpr BOOL MTIME_64 = true;
#endif

			UINT32 mode = *(UINT32 *)(statbuf + OFF_MODE);
			currentEntry.IsDirectory = ((mode & 0xF000) == 0x4000); // S_IFDIR

			INT64 fileSize = *(INT64 *)(statbuf + OFF_SIZE);
			currentEntry.Size = (fileSize > 0) ? (UINT64)fileSize : 0;

			INT64 mtime;
			if constexpr (MTIME_64)
				mtime = *(INT64 *)(statbuf + OFF_MTIME);
			else
				mtime = (INT64)(*(INT32 *)(statbuf + OFF_MTIME));
			currentEntry.LastModifiedTime = (UINT64)mtime;
			currentEntry.CreationTime = currentEntry.LastModifiedTime;
		}
		else
		{
			currentEntry.IsDirectory = (d->Type == DT_DIR);
			currentEntry.Size = 0;
			currentEntry.CreationTime = 0;
			currentEntry.LastModifiedTime = 0;
		}
	}
	currentEntry.Type = (UINT32)d->Type;
#endif
	currentEntry.IsDrive = false;
	currentEntry.IsHidden = (d->Name[0] == '.');
	currentEntry.IsSystem = false;
	currentEntry.IsReadOnly = false;

	bufferPosition += d->Reclen;

	return Result<VOID, Error>::Ok();
}

BOOL DirectoryIterator::IsValid() const
{
	return (SSIZE)handle >= 0;
}
