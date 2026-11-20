#include "platform/fs/file.h"
#include "platform/fs/posix/posix_path.h"
#include "core/memory/memory.h"
#include "core/string/string.h"
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

// --- lseek wrapper ---
// The offset is always 64-bit: a file position past 4 GiB must not wrap on
// 32-bit targets where the plain lseek ABI's off_t is pointer-sized.
// - Linux/Android 32-bit arches (riscv32, i386, armv7a, mips o32): use
//   sys_llseek, which takes 5 arguments (fd, offset_high, offset_low,
//   &result, whence), returns 0 on success and stores the new 64-bit
//   position through the result pointer. SYS_LSEEK64 names that syscall per
//   arch (riscv32's generic-table SYS_LSEEK (62) already IS sys_llseek;
//   i386/armv7a = 140, mips o32 = 4140).
// - FreeBSD i386: SYS_LSEEK (478) takes off_t (64-bit) split across two
//   consecutive 32-bit words (offset_lo, offset_hi) before whence; the
//   position comes back in eax (see the branch comment for the limits that
//   follow from that).
// - All other architectures use the standard 3-argument lseek (off_t is
//   64-bit on 64-bit targets; macOS/Solaris i386 keep the plain call — their
//   syscall tables expose no 64-bit llseek variant, so their 32-bit off_t
//   limit remains, as before this wrapper existed).
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_RISCV32) || defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A) || defined(ARCHITECTURE_MIPS))
static INT64 PosixLseek(USIZE fd, INT64 offset, INT32 whence)
{
	INT64 result = 0;
	USIZE offsetHigh = (USIZE)((UINT64)offset >> 32);
	USIZE offsetLow = (USIZE)((UINT64)offset & 0xFFFFFFFF);
	SSIZE ret = System::Call(SYS_LSEEK64, fd, offsetHigh, offsetLow, (USIZE)&result, (USIZE)whence);
	if (ret < 0)
		return (INT64)ret;
	return result;
}
#elif defined(PLATFORM_FREEBSD) && defined(ARCHITECTURE_I386)
static INT64 PosixLseek(USIZE fd, INT64 offset, INT32 whence)
{
	// Framing validated by CI (test-freebsd-i386 VM): (fd, offset_lo,
	// offset_hi, whence) with the position returned in eax. A compat32-style
	// alignment pad word and an edx:eax return capture were attempted here
	// and broke every seek in CI — do not re-add them without a FreeBSD
	// i386 system to verify against. Consequence: offsets whose low word has
	// bit 31 set (2–4 GiB) read back as negative and 4 GiB+ wraps on this
	// target; the >4 GiB regression test is gated off accordingly.
	USIZE offsetLow = (USIZE)((UINT64)offset & 0xFFFFFFFF);
	USIZE offsetHigh = (USIZE)((UINT64)offset >> 32);
	return (INT64)System::Call(SYS_LSEEK, fd, offsetLow, offsetHigh, (USIZE)whence);
}
#else
static INT64 PosixLseek(USIZE fd, INT64 offset, INT32 whence)
{
	return (INT64)System::Call(SYS_LSEEK, fd, (USIZE)offset, whence);
}
#endif

// --- Internal Constructor (trivial — never fails) ---
// The WPD parameters exist only for the Windows build's signature (they are
// defaulted at the call sites); POSIX has no portable-device stream state.
File::File(PVOID handle, UINT64 size, BOOL isWpd, PVOID wpdState)
	: fileHandle(handle), fileSize(size)
{
	(void)isWpd;
	(void)wpdState;
}

// --- Factory & Static Operations ---
Result<File, Error> File::Open(PCWCHAR path, INT32 flags)
{
	CHAR utf8Path[8192];
	NormalizePathToUtf8(path, Span<CHAR>(utf8Path));

	INT32 openFlags = 0;
	INT32 mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;

	// Access mode
	if ((flags & File::ModeRead) && (flags & File::ModeWrite))
		openFlags |= O_RDWR;
	else if (flags & File::ModeWrite)
		openFlags |= O_WRONLY;
	else
		openFlags |= O_RDONLY;

	// Creation/truncation flags
	if (flags & File::ModeCreate)
		openFlags |= O_CREAT;
	if (flags & File::ModeTruncate)
		openFlags |= O_TRUNC;
	if (flags & File::ModeAppend)
		openFlags |= O_APPEND;

	// 32-bit ABIs refuse >2 GiB files without this (EOVERFLOW) — the >4 GiB
	// seek support would be unreachable on real 32-bit hardware otherwise.
	// O_LARGEFILE is 0 (a no-op) on 64-bit ABIs.
	openFlags |= O_LARGEFILE;

	SSIZE fd;
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)) || defined(PLATFORM_SOLARIS)
	fd = System::Call(SYS_OPENAT, AT_FDCWD, (USIZE)utf8Path, openFlags, mode);
#else
	fd = System::Call(SYS_OPEN, (USIZE)utf8Path, openFlags, mode);
#endif

	if (fd < 0)
		return Result<File, Error>::Err(Error::Posix((UINT32)(-fd)), Error::Fs_OpenFailed);

	// Query file size via lseek (consistent with Windows/UEFI implementations)
	UINT64 size = 0;
	INT64 fileEnd = PosixLseek((USIZE)fd, 0, SEEK_END);
	if (fileEnd >= 0)
	{
		size = (UINT64)fileEnd;
		PosixLseek((USIZE)fd, 0, SEEK_SET);
	}

	return Result<File, Error>::Ok(File((PVOID)fd, size));
}

Result<VOID, Error> File::Delete(PCWCHAR path)
{
	CHAR utf8Path[8192];
	NormalizePathToUtf8(path, Span<CHAR>(utf8Path));

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)) || defined(PLATFORM_SOLARIS)
	SSIZE result = System::Call(SYS_UNLINKAT, AT_FDCWD, (USIZE)utf8Path, 0);
#else
	SSIZE result = System::Call(SYS_UNLINK, (USIZE)utf8Path);
#endif
	if (result == 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_DeleteFailed);
}

Result<VOID, Error> File::Exists(PCWCHAR path)
{
	CHAR utf8Path[8192];
	NormalizePathToUtf8(path, Span<CHAR>(utf8Path));

	UINT8 statbuf[144];

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)) || defined(PLATFORM_SOLARIS)
	SSIZE result = System::Call(SYS_FSTATAT, AT_FDCWD, (USIZE)utf8Path, (USIZE)statbuf, 0);
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
	SSIZE result = System::Call(SYS_STAT64, (USIZE)utf8Path, (USIZE)statbuf);
#else
	SSIZE result = System::Call(SYS_STAT, (USIZE)utf8Path, (USIZE)statbuf);
#endif
	if (result == 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_OpenFailed);
}

File::File(File &&other) noexcept : fileHandle((PVOID)INVALID_FD), fileSize(0)
{
	fileHandle = other.fileHandle;
	fileSize = other.fileSize;
	other.fileHandle = (PVOID)INVALID_FD;
	other.fileSize = 0;
}

File &File::operator=(File &&other) noexcept
{
	if (this != &other)
	{
		if (IsValid())
			(VOID)Close();
		fileHandle = other.fileHandle;
		fileSize = other.fileSize;
		other.fileHandle = (PVOID)INVALID_FD;
		other.fileSize = 0;
	}
	return *this;
}

BOOL File::IsValid() const
{
	SSIZE fd = (SSIZE)fileHandle;
	return fd >= 0;
}

VOID File::Close()
{
	if (IsValid())
	{
		System::Call(SYS_CLOSE, (USIZE)fileHandle);
		fileHandle = (PVOID)INVALID_FD;
		fileSize = 0;
	}
}

Result<UINT32, Error> File::Read(Span<UINT8> buffer)
{
	if (!IsValid())
		return Result<UINT32, Error>::Err(Error::Fs_ReadFailed);

	SSIZE result = System::Call(SYS_READ, (USIZE)fileHandle, (USIZE)buffer.Data(), buffer.Size());
	if (result >= 0)
		return Result<UINT32, Error>::Ok((UINT32)result);
	return Result<UINT32, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_ReadFailed);
}

Result<UINT32, Error> File::Write(Span<const UINT8> buffer)
{
	if (!IsValid())
		return Result<UINT32, Error>::Err(Error::Fs_WriteFailed);

	SSIZE result = System::Call(SYS_WRITE, (USIZE)fileHandle, (USIZE)buffer.Data(), buffer.Size());
	if (result >= 0)
		return Result<UINT32, Error>::Ok((UINT32)result);
	return Result<UINT32, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_WriteFailed);
}

Result<UINT64, Error> File::GetOffset() const
{
	if (!IsValid())
		return Result<UINT64, Error>::Err(Error::Fs_SeekFailed);

	INT64 result = PosixLseek((USIZE)fileHandle, 0, SEEK_CUR);
	if (result >= 0)
		return Result<UINT64, Error>::Ok((UINT64)result);
	return Result<UINT64, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_SeekFailed);
}

Result<VOID, Error> File::SetOffset(UINT64 absoluteOffset)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	INT64 result = PosixLseek((USIZE)fileHandle, (INT64)absoluteOffset, SEEK_SET);
	if (result >= 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_SeekFailed);
}

Result<VOID, Error> File::MoveOffset(INT64 relativeAmount, OffsetOrigin origin)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	INT32 whence = SEEK_CUR;
	if (origin == OffsetOrigin::Start)
		whence = SEEK_SET;
	else if (origin == OffsetOrigin::End)
		whence = SEEK_END;

	INT64 result = PosixLseek((USIZE)fileHandle, relativeAmount, whence);
	if (result >= 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_SeekFailed);
}
