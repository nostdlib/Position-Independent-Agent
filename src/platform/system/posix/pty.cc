/**
 * @file pty.cc
 * @brief Shared POSIX pseudo-terminal implementation
 *
 * @details Cross-platform PTY creation via /dev/ptmx with platform-specific
 * unlock and slave path discovery. Platform differences:
 * - Linux/Android: TIOCSPTLCK unlock + TIOCGPTN for slave number, /dev/pts/<N>
 * - Linux/Android MIPS64: O_NOCTTY value differs (0x800 vs 0x100)
 * - Linux/Android aarch64/riscv: SYS_OPENAT instead of SYS_OPEN
 * - macOS/iOS: TIOCPTYUNLK unlock + TIOCPTYGNAME for slave path
 * - FreeBSD: TIOCGPTN for slave number, /dev/pts/<N> (no unlock needed)
 * - Solaris: TIOCSPTLCK unlock + TIOCGPTN, /dev/pts/<N>
 * - Poll: SYS_PPOLL (Linux/Android), SYS_POLLSYS (Solaris), SYS_POLL (others)
 */

#include "platform/system/pty.h"
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
constexpr USIZE TIOCGPTN = 0x4004740f;
#elif defined(PLATFORM_SOLARIS)
constexpr USIZE TIOCSPTLCK = 0x40045431;
constexpr USIZE TIOCGPTN = 0x80045430;
#endif

// ============================================================================
// Local helpers
// ============================================================================

static SSIZE PtyOpen(const char *path, INT32 flags)
{
#if (defined(PLATFORM_FREEBSD) || (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)))
	return System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)flags, 0);
#else
	return System::Call(SYS_OPEN, (USIZE)path, (USIZE)flags, 0);
#endif
}

static BOOL PtyOpenPair(SSIZE &masterFd, SSIZE &slaveFd)
{
	masterFd = PtyOpen("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (masterFd < 0)
		return false;

	char slavePath[128];

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_SOLARIS)
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

#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
	(void)System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTYUNLK, 0);
	if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTYGNAME, (USIZE)slavePath) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
	}

#elif defined(PLATFORM_FREEBSD)
	INT32 ptyNum = 0;
	if (System::Call(SYS_IOCTL, (USIZE)masterFd, (USIZE)TIOCGPTN, (USIZE)&ptyNum) < 0)
	{
		System::Call(SYS_CLOSE, (USIZE)masterFd);
		return false;
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
