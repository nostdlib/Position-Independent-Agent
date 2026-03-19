/**
 * @file pty.cc
 * @brief Shared POSIX pseudo-terminal implementation
 *
 * @details Cross-platform PTY creation via /dev/ptmx with platform-specific
 * unlock and slave path discovery. Platform differences:
 * - Linux/Android: TIOCSPTLCK unlock + TIOCGPTN for slave number, /dev/pts/<N>
 * - Linux/Android MIPS64: O_NOCTTY value differs (0x800 vs 0x100)
 * - Linux/Android aarch64/riscv: SYS_OPENAT instead of SYS_OPEN
 * - FreeBSD: SYS_OPENAT, TIOCPTUNLK unlock + TIOCGPTN for slave number, /dev/pts/<N>
 * - macOS/iOS: TIOCPTYUNLK unlock + TIOCPTYGNAME for slave path
 * - Solaris: SYS_OPENAT, I_STR+UNLKPT unlock + fstat minor for slave number, /dev/pts/<N>
 * - Poll: SYS_PPOLL (Linux/Android), SYS_POLLSYS (Solaris), SYS_POLL (others)
 */

#include "platform/system/pty.h"
#include "logger.h"
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

// ============================================================================
// Platform-specific PTY ioctl codes
// ============================================================================

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
constexpr USIZE TIOCSPTLCK = 0x40045431;
constexpr USIZE TIOCGPTN = 0x80045430;
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
constexpr USIZE TIOCPTYUNLK = 0x20007452;
constexpr USIZE TIOCPTYGNAME = 0x40807453;
#elif defined(PLATFORM_FREEBSD)
constexpr USIZE TIOCPTUNLK = 0x20007410;
constexpr USIZE TIOCGPTN = 0x4004740f;
#elif defined(PLATFORM_SOLARIS)
// Solaris uses STREAMS ioctls, not Linux TIOCSPTLCK/TIOCGPTN
constexpr USIZE I_STR   = 0x5308;  // ('S' << 8) | 010
constexpr USIZE UNLKPT  = 0x5002;  // ('P' << 8) | 2
// st_rdev offset in Solaris struct stat
#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_AARCH64)
constexpr USIZE STAT_RDEV_OFFSET = 32;  // LP64: dev(8)+ino(8)+mode(4)+nlink(4)+uid(4)+gid(4)
constexpr USIZE STAT_BUF_SIZE    = 128;
#elif defined(ARCHITECTURE_I386)
constexpr USIZE STAT_RDEV_OFFSET = 36;  // ILP32: dev(4)+pad(12)+ino(4)+mode(4)+nlink(4)+uid(4)+gid(4)
constexpr USIZE STAT_BUF_SIZE    = 128;
#endif
#endif

// ============================================================================
// Local helpers
// ============================================================================

static SSIZE PtyOpen(const char *path, INT32 flags)
{
#if defined(PLATFORM_FREEBSD) || defined(PLATFORM_SOLARIS) || ((defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)))
	return System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)flags, 0);
#else
	return System::Call(SYS_OPEN, (USIZE)path, (USIZE)flags, 0);
#endif
}

static BOOL PtyOpenPair(SSIZE &masterFd, SSIZE &slaveFd)
{
	masterFd = PtyOpen("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (masterFd < 0)
	{
		LOG_ERROR("PTY: open /dev/ptmx failed (errno: %d)", (INT32)(-masterFd));
		return false;
	}

	char slavePath[128];

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	INT32 unlock = 0;
	if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCSPTLCK, (USIZE)&unlock) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
	}
	INT32 ptyNum = 0;
	if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCGPTN, (USIZE)&ptyNum) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
	}
	// Build "/dev/pts/<N>"
	const char prefix[] = "/dev/pts/";
	USIZE i = 0;
	for (; prefix[i]; i++)
		slavePath[i] = prefix[i];
	if (ptyNum == 0)
	{
		slavePath[i++] = '0';
	}
	else
	{
		char d[16];
		USIZE n = 0;
		INT32 v = ptyNum;
		while (v > 0)
		{
			d[n++] = '0' + (v % 10);
			v /= 10;
		}
		while (n > 0)
			slavePath[i++] = d[--n];
	}
	slavePath[i] = '\0';

#elif defined(PLATFORM_SOLARIS)
	// Unlock slave via STREAMS I_STR + UNLKPT
	struct { INT32 cmd; INT32 timout; INT32 len; PVOID dp; } strioctl;
	strioctl.cmd = (INT32)UNLKPT;
	strioctl.timout = 0;
	strioctl.len = 0;
	strioctl.dp = nullptr;
	if (System::Call(SYS_IOCTL, (USIZE)masterFd, I_STR, (USIZE)&strioctl) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
	}
	// Get slave PTY number via fstatat("/dev/fd/<N>") + minor(st_rdev)
	// Note: SYS_FSTAT is removed/repurposed on Oracle Solaris 11.4, use SYS_FSTATAT
	char fdPath[32];
	{
		const char fdPrefix[] = "/dev/fd/";
		USIZE j = 0;
		for (; fdPrefix[j]; j++) fdPath[j] = fdPrefix[j];
		SSIZE fd = masterFd;
		if (fd == 0) { fdPath[j++] = '0'; }
		else { char dd[16]; USIZE nn = 0; while (fd > 0) { dd[nn++] = '0' + (fd % 10); fd /= 10; } while (nn > 0) fdPath[j++] = dd[--nn]; }
		fdPath[j] = '\0';
	}
	UINT8 statBuf[STAT_BUF_SIZE] = {};
	if (System::Call(SYS_FSTATAT, (USIZE)AT_FDCWD, (USIZE)fdPath, (USIZE)statBuf, 0) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
	}
	USIZE rdev = 0;
#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_AARCH64)
	rdev = *(USIZE *)(statBuf + STAT_RDEV_OFFSET);
#elif defined(ARCHITECTURE_I386)
	rdev = *(UINT32 *)(statBuf + STAT_RDEV_OFFSET);
#endif
	INT32 ptyNum = (INT32)(rdev & 0x3ffff); // minor() on Solaris
	// Build "/dev/pts/<N>"
	const char prefix[] = "/dev/pts/";
	USIZE i = 0;
	for (; prefix[i]; i++)
		slavePath[i] = prefix[i];
	if (ptyNum == 0)
	{
		slavePath[i++] = '0';
	}
	else
	{
		char d[16];
		USIZE n = 0;
		INT32 v = ptyNum;
		while (v > 0)
		{
			d[n++] = '0' + (v % 10);
			v /= 10;
		}
		while (n > 0)
			slavePath[i++] = d[--n];
	}
	slavePath[i] = '\0';

#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
	{
		SSIZE unlkRet = System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTYUNLK, 0);
		if (unlkRet < 0)
		{
			LOG_ERROR("PTY: TIOCPTYUNLK failed (errno: %d)", (INT32)(-unlkRet));
			System::Call(SYS_CLOSE, (USIZE)masterFd);
			return false;
		}
	}
	{
		SSIZE nameRet = System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTYGNAME, (USIZE)slavePath);
		if (nameRet < 0)
		{
			LOG_ERROR("PTY: TIOCPTYGNAME failed (errno: %d)", (INT32)(-nameRet));
			System::Call(SYS_CLOSE, (USIZE)masterFd);
			return false;
		}
	}

#elif defined(PLATFORM_FREEBSD)
	{
		SSIZE unlkRet = System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTUNLK, 0);
		if (unlkRet < 0)
		{
			LOG_ERROR("PTY: TIOCPTUNLK failed (errno: %d)", (INT32)(-unlkRet));
			System::Call(SYS_CLOSE, (USIZE)masterFd);
			return false;
		}
	}
	INT32 ptyNum = 0;
	{
		SSIZE gptnRet = System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCGPTN, (USIZE)&ptyNum);
		if (gptnRet < 0)
		{
			LOG_ERROR("PTY: TIOCGPTN failed (errno: %d)", (INT32)(-gptnRet));
			System::Call(SYS_CLOSE, (USIZE)masterFd);
			return false;
		}
	}
	const char prefix[] = "/dev/pts/";
	USIZE i = 0;
	for (; prefix[i]; i++)
		slavePath[i] = prefix[i];
	if (ptyNum == 0)
	{
		slavePath[i++] = '0';
	}
	else
	{
		char d[16];
		USIZE n = 0;
		INT32 v = ptyNum;
		while (v > 0)
		{
			d[n++] = '0' + (v % 10);
			v /= 10;
		}
		while (n > 0)
			slavePath[i++] = d[--n];
	}
	slavePath[i] = '\0';
#endif

	slaveFd = PtyOpen(slavePath, O_RDWR | O_NOCTTY);
	if (slaveFd < 0)
	{
		LOG_ERROR("PTY: open slave failed (errno: %d)", (INT32)(-slaveFd));
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
	}
	return true;
}

// ============================================================================
// Pty::Create
// ============================================================================

Result<Pty, Error> Pty::Create() noexcept
{
	SSIZE master, slave;
	if (!PtyOpenPair(master, slave))
		return Result<Pty, Error>::Err(Error::Pty_CreateFailed);

	return Result<Pty, Error>::Ok(Pty(master, slave));
}

// ============================================================================
// Pty::Read
// ============================================================================

Result<USIZE, Error> Pty::Read(Span<UINT8> buffer) noexcept
{
	if (masterFd == INVALID_FD)
		return Result<USIZE, Error>::Err(Error::Pty_ReadFailed);

	SSIZE result = System::Call(SYS_READ, (USIZE)masterFd, (USIZE)buffer.Data(), buffer.Size());
	if (result < 0)
	{
		return Result<USIZE, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Pty_ReadFailed);
	}

	return Result<USIZE, Error>::Ok((USIZE)result);
}

// ============================================================================
// Pty::Write
// ============================================================================

Result<USIZE, Error> Pty::Write(Span<const UINT8> data) noexcept
{
	if (masterFd == INVALID_FD)
		return Result<USIZE, Error>::Err(Error::Pty_WriteFailed);

	SSIZE result = System::Call(SYS_WRITE, (USIZE)masterFd, (USIZE)data.Data(), data.Size());
	if (result < 0)
	{
		return Result<USIZE, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Pty_WriteFailed);
	}

	return Result<USIZE, Error>::Ok((USIZE)result);
}

// ============================================================================
// Pty::Poll
// ============================================================================

SSIZE Pty::Poll(SSIZE timeoutMs) noexcept
{
	if (masterFd == INVALID_FD)
		return -1;

	Pollfd pfd;
	pfd.Fd = (INT32)masterFd;
	pfd.Events = POLLIN;
	pfd.Revents = 0;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	Timespec ts;
	ts.Sec = timeoutMs / 1000;
	ts.Nsec = (timeoutMs % 1000) * 1000000;
	SSIZE ret = System::Call(SYS_PPOLL, (USIZE)&pfd, (USIZE)1, (USIZE)&ts, 0, 0);
#elif defined(PLATFORM_SOLARIS)
	Timespec ts;
	ts.Sec = timeoutMs / 1000;
	ts.Nsec = (timeoutMs % 1000) * 1000000;
	SSIZE ret = System::Call(SYS_POLLSYS, (USIZE)&pfd, (USIZE)1, (USIZE)&ts, 0);
#else
	SSIZE ret = System::Call(SYS_POLL, (USIZE)&pfd, (USIZE)1, (USIZE)timeoutMs);
#endif

	if (ret > 0 && (pfd.Revents & (POLLIN | POLLHUP | POLLERR)))
		return 1;
	return ret;
}

// ============================================================================
// Pty::CloseSlave
// ============================================================================

Result<void, Error> Pty::CloseSlave() noexcept
{
	if (slaveFd == INVALID_FD)
		return Result<void, Error>::Ok();

	System::Call(SYS_CLOSE, (USIZE)slaveFd);
	slaveFd = INVALID_FD;
	return Result<void, Error>::Ok();
}

// ============================================================================
// Pty::Close
// ============================================================================

Result<void, Error> Pty::Close() noexcept
{
	(void)CloseSlave();

	if (masterFd != INVALID_FD)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		masterFd = INVALID_FD;
	}

	return Result<void, Error>::Ok();
}
