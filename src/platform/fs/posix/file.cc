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
// On riscv32, SYS_LSEEK (62) maps to sys_llseek which takes 5 arguments:
// (fd, offset_high, offset_low, &result, whence) and returns 0 on success.
// On FreeBSD i386, SYS_LSEEK (478) takes off_t (64-bit) split across two
// consecutive 32-bit stack slots (offset_lo, offset_hi) before whence.
// All other architectures use the standard 3-argument lseek.
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_RISCV32)
static SSIZE PosixLseek(USIZE fd, SSIZE offset, INT32 whence)
{
	INT64 result = 0;
	INT64 offset64 = (INT64)offset;
	USIZE offsetHigh = (USIZE)((UINT64)offset64 >> 32);
	USIZE offsetLow = (USIZE)((UINT64)offset64 & 0xFFFFFFFF);
	SSIZE ret = System::Call(SYS_LSEEK, fd, offsetHigh, offsetLow, (USIZE)&result, (USIZE)whence);
	if (ret < 0)
		return ret;
	return (SSIZE)result;
}
#elif defined(PLATFORM_FREEBSD) && defined(ARCHITECTURE_I386)
static SSIZE PosixLseek(USIZE fd, SSIZE offset, INT32 whence)
{
	INT64 offset64 = (INT64)offset;
	USIZE offsetLow  = (USIZE)((UINT64)offset64 & 0xFFFFFFFF);
	USIZE offsetHigh = (USIZE)((UINT64)offset64 >> 32);
	return System::Call(SYS_LSEEK, fd, offsetLow, offsetHigh, (USIZE)whence);
}
#else
static SSIZE PosixLseek(USIZE fd, SSIZE offset, INT32 whence)
{
	return System::Call(SYS_LSEEK, fd, (USIZE)offset, whence);
}
#endif

// --- Internal Constructor (trivial — never fails) ---
File::File(PVOID handle, USIZE size) : fileHandle(handle), fileSize(size) {}

// --- Factory & Static Operations ---
Result<File, Error> File::Open(PCWCHAR path, INT32 flags)
{
	CHAR utf8Path[1024];
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

	SSIZE fd;
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)) || defined(PLATFORM_SOLARIS)
	fd = System::Call(SYS_OPENAT, AT_FDCWD, (USIZE)utf8Path, openFlags, mode);
#else
	fd = System::Call(SYS_OPEN, (USIZE)utf8Path, openFlags, mode);
#endif

	if (fd < 0)
		return Result<File, Error>::Err(Error::Posix((UINT32)(-fd)), Error::Fs_OpenFailed);

	// Query file size via lseek (consistent with Windows/UEFI implementations)
	USIZE size = 0;
	SSIZE fileEnd = PosixLseek((USIZE)fd, 0, SEEK_END);
	if (fileEnd >= 0)
	{
		size = (USIZE)fileEnd;
		PosixLseek((USIZE)fd, 0, SEEK_SET);
	}

	return Result<File, Error>::Ok(File((PVOID)fd, size));
}

Result<VOID, Error> File::Delete(PCWCHAR path)
{
	CHAR utf8Path[1024];
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
	CHAR utf8Path[1024];
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

Result<USIZE, Error> File::GetOffset() const
{
	if (!IsValid())
		return Result<USIZE, Error>::Err(Error::Fs_SeekFailed);

	SSIZE result = PosixLseek((USIZE)fileHandle, 0, SEEK_CUR);
	if (result >= 0)
		return Result<USIZE, Error>::Ok((USIZE)result);
	return Result<USIZE, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_SeekFailed);
}

Result<VOID, Error> File::SetOffset(USIZE absoluteOffset)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	SSIZE result = PosixLseek((USIZE)fileHandle, (SSIZE)absoluteOffset, SEEK_SET);
	if (result >= 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_SeekFailed);
}

Result<VOID, Error> File::MoveOffset(SSIZE relativeAmount, OffsetOrigin origin)
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);

	INT32 whence = SEEK_CUR;
	if (origin == OffsetOrigin::Start)
		whence = SEEK_SET;
	else if (origin == OffsetOrigin::End)
		whence = SEEK_END;

	SSIZE result = PosixLseek((USIZE)fileHandle, relativeAmount, whence);
	if (result >= 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_SeekFailed);
}
