/**
 * @file socket.cc
 * @brief Shared POSIX TCP socket implementation
 *
 * @details Unified BSD socket implementation for Linux, macOS, FreeBSD, and
 * Solaris. Platform differences are abstracted via static helper functions:
 * - Linux i386 uses multiplexed socketcall(2) instead of direct syscalls
 * - Linux i386/armv7a uses fcntl64 instead of fcntl
 * - Linux uses ppoll(2) for connection timeout
 * - Solaris uses SYS_so_socket and pollsys(2)
 * - macOS/FreeBSD use direct BSD syscalls and poll(2) with milliseconds
 */

#include "platform/socket/socket.h"
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
#include "core/types/ip_address.h"

// ============================================================================
// Platform-specific syscall helpers
// ============================================================================

// --- Socket creation ---
static SSIZE PosixSocket(INT32 domain, INT32 type, INT32 protocol)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_I386)
	USIZE args[3] = {(USIZE)domain, (USIZE)type, (USIZE)protocol};
	return System::Call(SYS_SOCKETCALL, SOCKOP_SOCKET, (USIZE)args);
#elif defined(PLATFORM_SOLARIS)
	// so_socket(family, type, protocol, devpath, version)
	// devpath=NULL, version=SOV_DEFAULT(1) for standard socket behavior
	return System::Call(SYS_SO_SOCKET, domain, type, protocol, (USIZE)0, (USIZE)1);
#else
	return System::Call(SYS_SOCKET, domain, type, protocol);
#endif
}

// --- Bind ---
static SSIZE PosixBind(SSIZE sockfd, const VOID *addr, UINT32 addrlen)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_I386)
	USIZE args[3] = {(USIZE)sockfd, (USIZE)addr, addrlen};
	return System::Call(SYS_SOCKETCALL, SOCKOP_BIND, (USIZE)args);
#else
	return System::Call(SYS_BIND, (USIZE)sockfd, (USIZE)addr, addrlen);
#endif
}

// --- Connect ---
static SSIZE PosixConnect(SSIZE sockfd, const VOID *addr, UINT32 addrlen)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_I386)
	USIZE args[3] = {(USIZE)sockfd, (USIZE)addr, addrlen};
	return System::Call(SYS_SOCKETCALL, SOCKOP_CONNECT, (USIZE)args);
#else
	return System::Call(SYS_CONNECT, (USIZE)sockfd, (USIZE)addr, addrlen);
#endif
}

// --- Send ---
static SSIZE PosixSend(SSIZE sockfd, const VOID *buf, USIZE len, INT32 flags)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_I386)
	USIZE args[4] = {(USIZE)sockfd, (USIZE)buf, len, (USIZE)flags};
	return System::Call(SYS_SOCKETCALL, SOCKOP_SEND, (USIZE)args);
#else
	return System::Call(SYS_SENDTO, (USIZE)sockfd, (USIZE)buf, len, flags, 0, 0);
#endif
}

// --- Recv ---
static SSIZE PosixRecv(SSIZE sockfd, VOID *buf, USIZE len, INT32 flags)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_I386)
	USIZE args[4] = {(USIZE)sockfd, (USIZE)buf, len, (USIZE)flags};
	return System::Call(SYS_SOCKETCALL, SOCKOP_RECV, (USIZE)args);
#else
	return System::Call(SYS_RECVFROM, (USIZE)sockfd, (USIZE)buf, len, flags, 0, 0);
#endif
}

// --- Getsockopt ---
static SSIZE PosixGetsockopt(SSIZE sockfd, INT32 level, INT32 optname, PVOID optval, UINT32 *optlen)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && defined(ARCHITECTURE_I386)
	USIZE args[5] = {(USIZE)sockfd, (USIZE)level, (USIZE)optname, (USIZE)optval, (USIZE)optlen};
	return System::Call(SYS_SOCKETCALL, SOCKOP_GETSOCKOPT, (USIZE)args);
#else
	return System::Call(SYS_GETSOCKOPT, (USIZE)sockfd, (USIZE)level, (USIZE)optname, (USIZE)optval, (USIZE)optlen);
#endif
}

// --- Fcntl ---
static SSIZE PosixFcntl(SSIZE fd, INT32 cmd, SSIZE arg = 0)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARMV7A))
	return System::Call(SYS_FCNTL64, (USIZE)fd, (USIZE)cmd, (USIZE)arg);
#else
	return System::Call(SYS_FCNTL, (USIZE)fd, (USIZE)cmd, (USIZE)arg);
#endif
}

// --- Poll for connection readiness ---
static SSIZE PosixPoll(Pollfd *pfd, USIZE nfds, SSIZE timeoutSec)
{
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
	Timespec timeout;
	timeout.Sec = timeoutSec;
	timeout.Nsec = 0;
	return System::Call(SYS_PPOLL, (USIZE)pfd, nfds, (USIZE)&timeout, 0, 0);
#elif defined(PLATFORM_SOLARIS)
	Timespec timeout;
	timeout.Sec = timeoutSec;
	timeout.Nsec = 0;
	return System::Call(SYS_POLLSYS, (USIZE)pfd, nfds, (USIZE)&timeout, 0);
#else
	// macOS / FreeBSD — poll() with millisecond timeout
	return System::Call(SYS_POLL, (USIZE)pfd, nfds, (USIZE)(timeoutSec * 1000));
#endif
}

// ============================================================================
// Socket implementation
// ============================================================================

Result<Socket, Error> Socket::Create(const IPAddress &ipAddress, UINT16 port)
{
	Socket sock(ipAddress, port);
	SSIZE fd = PosixSocket(SocketAddressHelper::GetAddressFamily(sock.ip), SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0)
		return Result<Socket, Error>::Err(Error::Posix((UINT32)(-fd)), Error::Socket_CreateFailed_Open);
	sock.handle = (PVOID)fd;
	return Result<Socket, Error>::Ok(static_cast<Socket &&>(sock));
}

Result<VOID, Error> Socket::Bind(const SockAddr &socketAddress, [[maybe_unused]] INT32 shareType)
{
	SSIZE  sockfd  = (SSIZE)handle;
	UINT32 addrLen = (ip.IsIPv6()) ? sizeof(SockAddr6) : sizeof(SockAddr);
	SSIZE  result  = PosixBind(sockfd, &socketAddress, addrLen);
	if (result != 0)
	{
		return Result<VOID, Error>::Err(
			Error::Posix((UINT32)(-result)),
			Error::Socket_BindFailed_Bind);
	}

	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Socket::Open()
{
	SSIZE sockfd = (SSIZE)handle;

	union
	{
		SockAddr  addr4;
		SockAddr6 addr6;
	} addrBuffer;

	UINT32 addrLen = SocketAddressHelper::PrepareAddress(ip, port, Span<UINT8>((UINT8 *)&addrBuffer, sizeof(addrBuffer)));
	if (addrLen == 0)
		return Result<VOID, Error>::Err(Error::Socket_OpenFailed_Connect);

	// Set socket to non-blocking for connect with timeout
	SSIZE flags = PosixFcntl(sockfd, F_GETFL);
	if (flags < 0)
		return Result<VOID, Error>::Err(Error::Posix((UINT32)(-flags)), Error::Socket_OpenFailed_Connect);

	SSIZE setResult = PosixFcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
	if (setResult < 0)
		return Result<VOID, Error>::Err(Error::Posix((UINT32)(-setResult)), Error::Socket_OpenFailed_Connect);

	SSIZE result = PosixConnect(sockfd, &addrBuffer, addrLen);
	if (result != 0 && (-(INT32)result) != EINPROGRESS)
	{
		(VOID)PosixFcntl(sockfd, F_SETFL, flags);
		return Result<VOID, Error>::Err(
			Error::Posix((UINT32)(-result)),
			Error::Socket_OpenFailed_Connect);
	}

	if (result != 0)
	{
		// Connect in progress — wait with 5-second timeout
		Pollfd pfd;
		pfd.Fd = (INT32)sockfd;
		pfd.Events = POLLOUT;
		pfd.Revents = 0;

		SSIZE pollResult = PosixPoll(&pfd, 1, 5);
		if (pollResult <= 0)
		{
			(VOID)PosixFcntl(sockfd, F_SETFL, flags);
			if (pollResult < 0)
				return Result<VOID, Error>::Err(
					Error::Posix((UINT32)(-pollResult)),
					Error::Socket_OpenFailed_Connect);
			return Result<VOID, Error>::Err(Error::Socket_OpenFailed_Connect);
		}

		// Check for connection error
		INT32 sockError = 0;
		UINT32 optLen = sizeof(sockError);
		SSIZE gsoResult = PosixGetsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sockError, &optLen);
		if (gsoResult < 0 || sockError != 0)
		{
			(VOID)PosixFcntl(sockfd, F_SETFL, flags);
			UINT32 errCode = (sockError != 0) ? (UINT32)sockError : (UINT32)(-gsoResult);
			return Result<VOID, Error>::Err(
				Error::Posix(errCode),
				Error::Socket_OpenFailed_Connect);
		}
	}

	// Restore blocking mode
	(VOID)PosixFcntl(sockfd, F_SETFL, flags);
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Socket::Close()
{
	SSIZE sockfd = (SSIZE)handle;
	SSIZE result = System::Call(SYS_CLOSE, (USIZE)sockfd);
	handle = nullptr;
	if (result < 0)
		return Result<VOID, Error>::Err(Error::Posix((UINT32)(-result)), Error::Socket_CloseFailed_Close);
	return Result<VOID, Error>::Ok();
}

Result<SSIZE, Error> Socket::Read(Span<CHAR> buffer)
{
	SSIZE sockfd = (SSIZE)handle;
	SSIZE result = PosixRecv(sockfd, (PVOID)buffer.Data(), buffer.Size(), 0);
	if (result < 0)
	{
		return Result<SSIZE, Error>::Err(
			Error::Posix((UINT32)(-result)),
			Error::Socket_ReadFailed_Recv);
	}

	return Result<SSIZE, Error>::Ok(result);
}

Result<UINT32, Error> Socket::Write(Span<const CHAR> buffer)
{
	SSIZE  sockfd    = (SSIZE)handle;
	UINT32 totalSent = 0;

	while (totalSent < buffer.Size())
	{
		SSIZE sent = PosixSend(sockfd, (const CHAR *)buffer.Data() + totalSent,
		                        buffer.Size() - totalSent, 0);
		if (sent <= 0)
		{
			if (sent < 0)
				return Result<UINT32, Error>::Err(
					Error::Posix((UINT32)(-sent)),
					Error::Socket_WriteFailed_Send);
			return Result<UINT32, Error>::Err(
				Error::Socket_WriteFailed_Send);
		}

		totalSent += (UINT32)sent;
	}

	return Result<UINT32, Error>::Ok(totalSent);
}
