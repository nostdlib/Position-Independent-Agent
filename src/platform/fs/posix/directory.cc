#include "platform/fs/directory.h"
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

// =============================================================================
// Directory Implementation
// =============================================================================

Result<VOID, Error> Directory::Create(PCWCHAR path)
{
	CHAR utf8Path[1024];
	NormalizePathToUtf8(path, Span<CHAR>(utf8Path));

	// Mode 0755 (rwxr-xr-x)
	INT32 mode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)) || defined(PLATFORM_SOLARIS)
	SSIZE result = System::Call(SYS_MKDIRAT, AT_FDCWD, (USIZE)utf8Path, mode);
#else
	SSIZE result = System::Call(SYS_MKDIR, (USIZE)utf8Path, mode);
#endif
	if (result == 0 || result == -EEXIST)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_CreateDirFailed);
}

Result<VOID, Error> Directory::Delete(PCWCHAR path)
{
	CHAR utf8Path[1024];
	NormalizePathToUtf8(path, Span<CHAR>(utf8Path));

#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_FREEBSD)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)) || defined(PLATFORM_SOLARIS)
	SSIZE result = System::Call(SYS_UNLINKAT, AT_FDCWD, (USIZE)utf8Path, AT_REMOVEDIR);
#else
	SSIZE result = System::Call(SYS_RMDIR, (USIZE)utf8Path);
#endif
	if (result == 0)
		return Result<VOID, Error>::Ok();
	return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Fs_DeleteDirFailed);
}
