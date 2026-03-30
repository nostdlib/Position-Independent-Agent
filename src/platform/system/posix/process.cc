/**
 * @file process.cc
 * @brief Shared POSIX process management implementation
 *
 * @details Cross-platform process creation via fork+execve with optional I/O
 * redirection, plus wait4/kill for lifecycle management. Platform differences:
 * - Linux aarch64/riscv: clone(SIGCHLD) for fork, dup3 for dup2
 * - Linux riscv32: waitid instead of wait4 (wait4 returns ENOSYS)
 * - Solaris: SYS_forksys for fork, fcntl(F_DUP2FD) for dup2,
 *   SYS_pgrpsys for setsid, waitid instead of wait4
 * - macOS/iOS/Solaris x86: custom fork asm to handle BSD rval[1] convention
 *   (raw fork syscall returns parent PID in rax/x0 for child; rdx/x1 = 1
 *   distinguishes child from parent)
 * - FreeBSD: kernel sets rval[0] = 0 for child in fork_exit, no fixup needed
 */

#include "platform/system/process.h"
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

#include "core/memory/memory.h"

// ============================================================================
// Local POSIX helpers (not exposed in header)
// ============================================================================

/**
 * PosixFork - Fork the current process
 *
 * @details On macOS/iOS and Solaris x86, the raw fork syscall follows the BSD
 * convention: both parent and child receive a non-zero PID in rax/x0 (child PID
 * for parent, parent PID for child). The secondary return value (rdx/x1) is 0
 * for the parent and 1 for the child. Standard System::Call only returns rax/x0,
 * so we use custom inline asm to check rdx/x1 and return 0 for the child.
 *
 * FreeBSD's kernel explicitly sets rval[0] = 0 for the child in fork_exit(),
 * so no fixup is needed there. Linux always returns 0 for the child.
 *
 * @return Child PID in parent, 0 in child, negative errno on error
 */
static NOINLINE SSIZE PosixFork() noexcept
{
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
	// macOS/iOS: raw fork returns parent PID in x0/rax for child;
	// x1/rdx = 1 for child, 0 for parent. Must check secondary return value.
#if defined(ARCHITECTURE_AARCH64)
	register USIZE x0 __asm__("x0");
	register USIZE x1 __asm__("x1");
	register USIZE x16 __asm__("x16") = SYS_FORK;
	__asm__ volatile(
		"svc #0x80\n"
		"b.cs 2f\n"
		"cbz x1, 1f\n"
		"mov x0, #0\n"
		"1:\n"
		"b 3f\n"
		"2:\n"
		"neg x0, x0\n"
		"3:\n"
		: "=r"(x0), "=r"(x1)
		: "r"(x16)
		: "memory", "cc"
	);
	return (SSIZE)x0;
#elif defined(ARCHITECTURE_X86_64)
	register USIZE r_rax __asm__("rax") = SYS_FORK;
	register USIZE r_rdx __asm__("rdx");
	__asm__ volatile(
		"syscall\n"
		"jc 2f\n"
		"testq %%rdx, %%rdx\n"
		"jz 1f\n"
		"xorq %%rax, %%rax\n"
		"1:\n"
		"jmp 3f\n"
		"2:\n"
		"negq %%rax\n"
		"3:\n"
		: "+r"(r_rax), "=r"(r_rdx)
		:
		: "rcx", "r11", "memory", "cc"
	);
	return (SSIZE)r_rax;
#endif

#elif defined(PLATFORM_SOLARIS)
	// Solaris: forksys(FORKSYS_FORK, 0) follows the same BSD rval[1] convention.
#if defined(ARCHITECTURE_X86_64)
	register USIZE r_rdi __asm__("rdi") = FORKSYS_FORK;
	register USIZE r_rsi __asm__("rsi") = 0;
	register USIZE r_rax __asm__("rax") = SYS_FORKSYS;
	register USIZE r_rdx __asm__("rdx");
	__asm__ volatile(
		"syscall\n"
		"jc 2f\n"
		"testq %%rdx, %%rdx\n"
		"jz 1f\n"
		"xorq %%rax, %%rax\n"
		"1:\n"
		"jmp 3f\n"
		"2:\n"
		"negq %%rax\n"
		"3:\n"
		: "+r"(r_rax), "=r"(r_rdx)
		: "r"(r_rdi), "r"(r_rsi)
		: "rcx", "r11", "memory", "cc"
	);
	return (SSIZE)r_rax;
#elif defined(ARCHITECTURE_I386)
	SSIZE ret;
	register USIZE r_ebx __asm__("ebx") = FORKSYS_FORK;
	register USIZE r_ecx __asm__("ecx") = 0;
	__asm__ volatile(
		"pushl %%ecx\n"
		"pushl %%ebx\n"
		"pushl $0\n"
		"int $0x91\n"
		"jc 2f\n"
		"testl %%edx, %%edx\n"
		"jz 1f\n"
		"xorl %%eax, %%eax\n"
		"1:\n"
		"jmp 3f\n"
		"2:\n"
		"negl %%eax\n"
		"3:\n"
		"addl $12, %%esp\n"
		: "=a"(ret)
		: "a"(SYS_FORKSYS), "r"(r_ebx), "r"(r_ecx)
		: "edx", "memory", "cc"
	);
	return ret;
#elif defined(ARCHITECTURE_AARCH64)
	// Solaris aarch64 is compile-only (no runner); use standard System::Call
	return System::Call(SYS_FORKSYS, FORKSYS_FORK, 0);
#endif

#elif (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))
	constexpr USIZE SIGCHLD_VAL = 17;
	return System::Call(SYS_CLONE, SIGCHLD_VAL, 0, 0, 0, 0);
#else
	return System::Call(SYS_FORK);
#endif
}

static SSIZE PosixDup2(SSIZE oldfd, SSIZE newfd) noexcept
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))
	return System::Call(SYS_DUP3, (USIZE)oldfd, (USIZE)newfd, 0);
#elif defined(PLATFORM_SOLARIS)
	return System::Call(SYS_FCNTL, (USIZE)oldfd, (USIZE)F_DUP2FD, (USIZE)newfd);
#else
	return System::Call(SYS_DUP2, (USIZE)oldfd, (USIZE)newfd);
#endif
}

static SSIZE PosixSetsid() noexcept
{
#if defined(PLATFORM_SOLARIS)
	return System::Call(SYS_PGRPSYS, PGRPSYS_SETSID);
#else
	return System::Call(SYS_SETSID);
#endif
}

// Linux riscv32 waitid constants (different values from Solaris)
#if defined(PLATFORM_LINUX) && defined(ARCHITECTURE_RISCV32)
constexpr INT32 LINUX_P_PID = 1;
constexpr INT32 LINUX_WEXITED = 4;
#endif

// TIOCSCTTY - set controlling terminal (needed for PTY shells)
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
constexpr USIZE TIOCSCTTY = 0x540E;
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
constexpr USIZE TIOCSCTTY = 0x20007461;
#endif

// ============================================================================
// Process::Create
// ============================================================================

Result<Process, Error> Process::Create(
	const CHAR *path,
	const CHAR *const args[],
	SSIZE stdinFd,
	SSIZE stdoutFd,
	SSIZE stderrFd) noexcept
{
	if (path == nullptr || args == nullptr)
	{
		return Result<Process, Error>::Err(Error::Process_CreateFailed);
	}

	SSIZE pid = PosixFork();
	if (pid < 0)
	{
		return Result<Process, Error>::Err(
			Error::Posix((UINT32)(-pid)), Error::Process_CreateFailed);
	}

	if (pid == 0)
	{
		// Child process
		BOOL hasRedirect = (stdinFd != -1 || stdoutFd != -1 || stderrFd != -1);

		if (hasRedirect)
		{
			// Create new session and set controlling terminal
			(VOID)PosixSetsid();
#if !defined(PLATFORM_SOLARIS)
			// Set the PTY slave as controlling terminal (no-op if not a PTY)
			if (stdinFd != -1)
				(VOID)System::Call(SYS_IOCTL, (USIZE)stdinFd, TIOCSCTTY, 0);
#endif

			// Redirect each fd that was specified
			if (stdinFd != -1 && PosixDup2(stdinFd, STDIN_FILENO) < 0)
				System::Call(SYS_EXIT, 1);
			if (stdoutFd != -1 && PosixDup2(stdoutFd, STDOUT_FILENO) < 0)
				System::Call(SYS_EXIT, 1);
			if (stderrFd != -1 && PosixDup2(stderrFd, STDERR_FILENO) < 0)
				System::Call(SYS_EXIT, 1);

			// Close original fds if they are not standard fds
			// (avoid closing if they were redirected to a standard fd)
			SSIZE fds[3] = {stdinFd, stdoutFd, stderrFd};
			for (INT32 i = 0; i < 3; ++i)
			{
				if (fds[i] > STDERR_FILENO)
				{
					// Check not already used as another redirect target
					BOOL stillNeeded = false;
					for (INT32 j = i + 1; j < 3; ++j)
					{
						if (fds[j] == fds[i])
						{
							stillNeeded = true;
							break;
						}
					}
					if (!stillNeeded)
						System::Call(SYS_CLOSE, (USIZE)fds[i]);
				}
			}
		}

		// Build envp (empty environment)
		USIZE envp[1];
		envp[0] = 0;

		// Execute — does not return on success
		System::Call(SYS_EXECVE, (USIZE)path, (USIZE)args, (USIZE)envp);

		// If execve returned, exit child
		System::Call(SYS_EXIT, 1);
	}

	// Parent — return Process with child PID
	return Result<Process, Error>::Ok(Process(pid));
}

// ============================================================================
// Process::Wait
// ============================================================================

Result<SSIZE, Error> Process::Wait() noexcept
{
	if (!IsValid())
		return Result<SSIZE, Error>::Err(Error::Process_WaitFailed);

#if defined(PLATFORM_SOLARIS) || (defined(PLATFORM_LINUX) && defined(ARCHITECTURE_RISCV32))
	// Solaris and Linux riscv32 use waitid(P_PID, pid, &siginfo, WEXITED)
	UINT8 siginfo[256];
	Memory::Zero(siginfo, sizeof(siginfo));
#if defined(PLATFORM_SOLARIS)
	SSIZE result = System::Call(SYS_WAITID, (USIZE)P_PID, (USIZE)id, (USIZE)siginfo, (USIZE)WEXITED);
#else
	SSIZE result = System::Call(SYS_WAITID, (USIZE)LINUX_P_PID, (USIZE)id, (USIZE)siginfo, (USIZE)LINUX_WEXITED, 0);
#endif
	if (result < 0)
	{
		return Result<SSIZE, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Process_WaitFailed);
	}
	// Extract si_status from siginfo_t
	// Solaris (all archs): offset 24 (si_signo + si_code + si_errno + pid + uid + utime = 24)
	// Linux riscv32: offset 20 (si_signo + si_errno + si_code + pid + uid = 20)
#if defined(PLATFORM_SOLARIS)
	INT32 exitCode = *(INT32 *)(siginfo + 24);
#else
	INT32 exitCode = *(INT32 *)(siginfo + 20);
#endif
	id = INVALID_ID;
	return Result<SSIZE, Error>::Ok((SSIZE)exitCode);
#else
	// All other POSIX platforms use wait4
	INT32 status = 0;
	SSIZE result = System::Call(SYS_WAIT4, (USIZE)id, (USIZE)&status, 0, 0);
	if (result < 0)
	{
		return Result<SSIZE, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Process_WaitFailed);
	}
	id = INVALID_ID;
	// WEXITSTATUS: bits 15..8 of status
	SSIZE exitCode = (SSIZE)((status >> 8) & 0xFF);
	return Result<SSIZE, Error>::Ok(exitCode);
#endif
}

// ============================================================================
// Process::Terminate
// ============================================================================

Result<VOID, Error> Process::Terminate() noexcept
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Process_TerminateFailed);

	SSIZE result = System::Call(SYS_KILL, (USIZE)id, (USIZE)SIGKILL);
	if (result < 0)
	{
		return Result<VOID, Error>::Err(
			Error::Posix((UINT32)(-result)), Error::Process_TerminateFailed);
	}
	return Result<VOID, Error>::Ok();
}

// ============================================================================
// Process::IsRunning
// ============================================================================

BOOL Process::IsRunning() const noexcept
{
	if (!IsValid())
		return false;

	// kill(pid, 0) checks if process exists without sending a signal
	SSIZE result = System::Call(SYS_KILL, (USIZE)id, 0);
	return result >= 0;
}

// ============================================================================
// Process::Close
// ============================================================================

Result<VOID, Error> Process::Close() noexcept
{
	if (!IsValid())
		return Result<VOID, Error>::Ok();

	// Try to reap zombie (non-blocking) to avoid resource leak
#if defined(PLATFORM_SOLARIS) || (defined(PLATFORM_LINUX) && defined(ARCHITECTURE_RISCV32))
	UINT8 siginfo[256];
	Memory::Zero(siginfo, sizeof(siginfo));
#if defined(PLATFORM_SOLARIS)
	(VOID)System::Call(SYS_WAITID, (USIZE)P_PID, (USIZE)id, (USIZE)siginfo, (USIZE)(WEXITED | WNOHANG));
#else
	(VOID)System::Call(SYS_WAITID, (USIZE)LINUX_P_PID, (USIZE)id, (USIZE)siginfo, (USIZE)(LINUX_WEXITED | WNOHANG), 0);
#endif
#else
	INT32 status = 0;
	(VOID)System::Call(SYS_WAIT4, (USIZE)id, (USIZE)&status, (USIZE)WNOHANG, 0);
#endif

	id = INVALID_ID;
	return Result<VOID, Error>::Ok();
}
