/**
 * @file socket.h
 * @brief TCP Stream Socket Abstraction
 *
 * @details Cross-platform TCP stream socket implementation providing a unified
 * interface over platform-specific networking primitives. Supports both IPv4
 * (RFC 791) and IPv6 (RFC 8200) transport via the Transmission Control Protocol
 * (RFC 9293).
 *
 * Platform backends:
 * - **Windows:** AFD (Auxiliary Function Driver) IOCTLs via NT Native API
 *   (ZwCreateFile, ZwDeviceIoControlFile on \\Device\\Afd\\Endpoint)
 * - **Linux:** Direct syscalls — socket(2), connect(2), sendto(2), recvfrom(2)
 *   (i386 uses multiplexed socketcall(2))
 * - **macOS:** BSD syscalls — socket(2), connect(2), sendto(2), recvfrom(2)
 * - **UEFI:** EFI_TCP4_PROTOCOL / EFI_TCP6_PROTOCOL via Service Binding
 *
 * The Socket class follows RAII ownership semantics: the factory method
 * Create() allocates the underlying OS handle, and the destructor releases
 * it. Copy is deleted; move transfers ownership.
 *
 * @note All operations are position-independent — no static imports, no CRT,
 * no .rdata dependencies.
 *
 * @see RFC 9293 — Transmission Control Protocol (TCP)
 *      https://datatracker.ietf.org/doc/html/rfc9293
 * @see RFC 791 — Internet Protocol (IPv4)
 *      https://datatracker.ietf.org/doc/html/rfc791
 * @see RFC 8200 — Internet Protocol, Version 6 (IPv6)
 *      https://datatracker.ietf.org/doc/html/rfc8200
 * @see RFC 3493 — Basic Socket Interface Extensions for IPv6
 *      https://datatracker.ietf.org/doc/html/rfc3493
 *
 * @ingroup network
 *
 * @defgroup socket TCP Socket
 * @ingroup network
 * @{
 */

#pragma once

#include "core/core.h"
#include "core/types/error.h"

/** @name Address Families
 * @brief Protocol family constants for socket creation.
 * @see RFC 3493 Section 3.2 — Socket Address Structures
 *      https://datatracker.ietf.org/doc/html/rfc3493#section-3.2
 * @{
 */
#define AF_INET 2   ///< IPv4 address family (RFC 791)
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_UEFI)
#define AF_INET6 23 ///< IPv6 address family — Windows/UEFI value (RFC 8200)
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
#define AF_INET6 30 ///< IPv6 address family — macOS/iOS/BSD value (RFC 8200)
#elif defined(PLATFORM_SOLARIS)
#define AF_INET6 26 ///< IPv6 address family — Solaris/illumos value (RFC 8200)
#elif defined(PLATFORM_FREEBSD)
#define AF_INET6 28 ///< IPv6 address family — FreeBSD value (RFC 8200)
#else
#define AF_INET6 10 ///< IPv6 address family — Linux value (RFC 8200)
#endif
/** @} */

/** @name Socket Types
 * @brief Socket type constants for stream and datagram communication.
 * @see RFC 9293 Section 3.1 — Header Format (SOCK_STREAM for TCP)
 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.1
 * @{
 */
#if defined(PLATFORM_SOLARIS) || defined(PLATFORM_LINUX_MIPS64)
#define SOCK_STREAM 2 ///< Stream socket — Solaris/MIPS value (TCP)
#define SOCK_DGRAM 1  ///< Datagram socket — Solaris/MIPS value (UDP)
#else
#define SOCK_STREAM 1 ///< Stream socket — reliable, ordered, connection-oriented (TCP)
#define SOCK_DGRAM 2  ///< Datagram socket — unreliable, connectionless (UDP)
#endif
/** @} */

/** @name Shutdown Modes
 * @brief Shutdown direction constants for Socket::Close().
 * @see RFC 9293 Section 3.6 — Closing a Connection
 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.6
 * @{
 */
#define SHUT_RD 0    ///< Shut down the reading side of the socket
#define SHUT_WR 1    ///< Shut down the writing side of the socket (sends FIN)
#define SHUT_RDWR 2  ///< Shut down both reading and writing
/** @} */

/**
 * @struct SockAddr
 * @brief IPv4 socket address structure (sockaddr_in equivalent)
 *
 * @details Mirrors the POSIX sockaddr_in structure used to specify an IPv4
 * endpoint for socket operations (bind, connect). Fields are stored in
 * network byte order where required by the protocol.
 *
 * @see RFC 791 — Internet Protocol (IPv4 addressing)
 *      https://datatracker.ietf.org/doc/html/rfc791
 */
struct SockAddr
{
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
	UINT8 SinLen;      ///< Structure length (BSD-specific, must be sizeof(SockAddr))
	UINT8 SinFamily;   ///< Address family (AF_INET) — 1 byte on BSD
#else
	UINT16 SinFamily;  ///< Address family (AF_INET)
#endif
	UINT16 SinPort;    ///< Port number in network byte order
	UINT32 SinAddr;    ///< IPv4 address in network byte order
	CHAR SinZero[8];   ///< Padding to match sockaddr size (must be zeroed)
};

/**
 * @struct SockAddr6
 * @brief IPv6 socket address structure (sockaddr_in6 equivalent)
 *
 * @details Mirrors the POSIX sockaddr_in6 structure defined in RFC 3493
 * Section 3.3. Used to specify an IPv6 endpoint for socket operations.
 *
 * @see RFC 3493 Section 3.3 — Socket Address Structure for IPv6
 *      https://datatracker.ietf.org/doc/html/rfc3493#section-3.3
 * @see RFC 8200 — Internet Protocol, Version 6 (IPv6)
 *      https://datatracker.ietf.org/doc/html/rfc8200
 */
struct SockAddr6
{
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
	UINT8 Sin6Len;        ///< Structure length (BSD-specific, must be sizeof(SockAddr6))
	UINT8 Sin6Family;     ///< Address family (AF_INET6) — 1 byte on BSD
#else
	UINT16 Sin6Family;    ///< Address family (AF_INET6)
#endif
	UINT16 Sin6Port;      ///< Port number in network byte order
	UINT32 Sin6Flowinfo;  ///< IPv6 flow information (RFC 8200 Section 7)
	UINT8 Sin6Addr[16];   ///< 128-bit IPv6 address
	UINT32 Sin6ScopeId;  ///< Scope ID for link-local addresses (RFC 3493 Section 3.3)
};

/**
 * @class SocketAddressHelper
 * @brief Utility class for preparing socket address structures from IPAddress
 *
 * @details Converts high-level IPAddress objects into the low-level SockAddr /
 * SockAddr6 structures required by platform socket APIs. Handles IPv4/IPv6
 * dispatch, byte-order conversion, and zero-initialization.
 *
 * @see RFC 3493 Section 3.2 — Socket Address Structures
 *      https://datatracker.ietf.org/doc/html/rfc3493#section-3.2
 */
class SocketAddressHelper
{
public:
	/**
	 * @brief Prepares a socket address for connect/bind operations
	 *
	 * @details Populates a SockAddr (IPv4) or SockAddr6 (IPv6) structure in
	 * the caller-provided buffer based on the IP address version. The port is
	 * converted to network byte order (big-endian) per RFC 9293 Section 3.1.
	 * The address structure is zero-initialized before populating.
	 *
	 * @param ip Source IP address (IPv4 or IPv6)
	 * @param port Destination port number in host byte order
	 * @param addrBuffer Output buffer (must be at least sizeof(SockAddr) or sizeof(SockAddr6))
	 * @return Size of the prepared address structure in bytes, or 0 if the buffer is too small
	 *
	 * @see RFC 9293 Section 3.1 — Header Format (port encoding)
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.1
	 */
	static UINT32 PrepareAddress(const IPAddress &ip, UINT16 port, Span<UINT8> addrBuffer)
	{
		if (ip.IsIPv6())
		{
			if (addrBuffer.Size() < sizeof(SockAddr6))
				return 0;

			SockAddr6 *addr6 = (SockAddr6 *)addrBuffer.Data();
			Memory::Zero(addr6, sizeof(SockAddr6));
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
			addr6->Sin6Len = sizeof(SockAddr6);
			addr6->Sin6Family = (UINT8)AF_INET6;
#else
			addr6->Sin6Family = AF_INET6;
#endif
			addr6->Sin6Port = ByteOrder::Swap16(port);
			addr6->Sin6Flowinfo = 0;
			addr6->Sin6ScopeId = 0;

			const UINT8 *ipv6Addr = ip.ToIPv6();
			if (ipv6Addr != nullptr)
			{
				Memory::Copy(addr6->Sin6Addr, ipv6Addr, 16);
			}

			return sizeof(SockAddr6);
		}
		else
		{
			if (addrBuffer.Size() < sizeof(SockAddr))
				return 0;

			SockAddr *addr = (SockAddr *)addrBuffer.Data();
			Memory::Zero(addr, sizeof(SockAddr));
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
			addr->SinLen = sizeof(SockAddr);
			addr->SinFamily = (UINT8)AF_INET;
#else
			addr->SinFamily = AF_INET;
#endif
			addr->SinPort = ByteOrder::Swap16(port);
			addr->SinAddr = ip.ToIPv4();

			return sizeof(SockAddr);
		}
	}

	/**
	 * @brief Prepares a wildcard bind address (INADDR_ANY / in6addr_any)
	 *
	 * @details Creates a socket address with a zeroed IP field suitable for
	 * binding to all local interfaces. On IPv4 this is INADDR_ANY (0.0.0.0),
	 * on IPv6 this is in6addr_any (::). Used internally before connect() on
	 * platforms that require an explicit bind (e.g., Windows AFD).
	 *
	 * @param isIPv6 true to prepare an IPv6 wildcard address, false for IPv4
	 * @param port Local port number in host byte order (0 for ephemeral)
	 * @param addrBuffer Output buffer (must be at least sizeof(SockAddr) or sizeof(SockAddr6))
	 * @return Size of the prepared address structure in bytes, or 0 if the buffer is too small
	 *
	 * @see RFC 3493 Section 3.2 — Socket Address Structures
	 *      https://datatracker.ietf.org/doc/html/rfc3493#section-3.2
	 */
	static UINT32 PrepareBindAddress(BOOL isIPv6, UINT16 port, Span<UINT8> addrBuffer)
	{
		if (isIPv6)
		{
			if (addrBuffer.Size() < sizeof(SockAddr6))
				return 0;

			SockAddr6 *addr6 = (SockAddr6 *)addrBuffer.Data();
			Memory::Zero(addr6, sizeof(SockAddr6));
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
			addr6->Sin6Len = sizeof(SockAddr6);
			addr6->Sin6Family = (UINT8)AF_INET6;
#else
			addr6->Sin6Family = AF_INET6;
#endif
			addr6->Sin6Port = ByteOrder::Swap16(port);

			return sizeof(SockAddr6);
		}
		else
		{
			if (addrBuffer.Size() < sizeof(SockAddr))
				return 0;

			SockAddr *addr = (SockAddr *)addrBuffer.Data();
			Memory::Zero(addr, sizeof(SockAddr));
#if defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_FREEBSD)
			addr->SinLen = sizeof(SockAddr);
			addr->SinFamily = (UINT8)AF_INET;
#else
			addr->SinFamily = AF_INET;
#endif
			addr->SinPort = ByteOrder::Swap16(port);

			return sizeof(SockAddr);
		}
	}

	/**
	 * @brief Returns the address family constant for an IP address
	 *
	 * @details Maps IPAddress version to the platform-appropriate AF_INET or
	 * AF_INET6 constant for use with socket creation and address structures.
	 *
	 * @param ip IP address to query
	 * @return AF_INET6 for IPv6 addresses, AF_INET otherwise
	 */
	static constexpr INT32 GetAddressFamily(const IPAddress &ip)
	{
		return ip.IsIPv6() ? AF_INET6 : AF_INET;
	}
};

/**
 * @class Socket
 * @brief RAII TCP stream socket for IPv4 and IPv6 connections
 *
 * @details Provides a cross-platform TCP (SOCK_STREAM) socket implementing the
 * client side of the TCP connection lifecycle defined in RFC 9293:
 *
 * 1. **Create()** — allocates the underlying OS socket handle (CLOSED state)
 * 2. **Open()** — initiates the TCP three-way handshake (SYN → SYN-ACK → ACK)
 *    transitioning to the ESTABLISHED state (RFC 9293 Section 3.5)
 * 3. **Read()/Write()** — exchange data on the established connection
 *    (RFC 9293 Section 3.8)
 * 4. **Close()** — releases the socket handle and associated resources
 *    (RFC 9293 Section 3.6)
 *
 * The socket follows RAII ownership: Create() is the only way to obtain a valid
 * socket, the destructor calls Close() automatically, copy is deleted, and move
 * transfers ownership (nullifying the source).
 *
 * @par Platform implementations:
 * - **Windows:** Opens \\Device\\Afd\\Endpoint via ZwCreateFile, performs
 *   bind/connect/send/recv through AFD IOCTLs (IOCTL_AFD_BIND,
 *   IOCTL_AFD_CONNECT, IOCTL_AFD_SEND, IOCTL_AFD_RECV)
 * - **Linux:** Direct socket(2)/connect(2)/sendto(2)/recvfrom(2) syscalls
 *   (i386 uses multiplexed socketcall(2))
 * - **macOS:** BSD socket(2)/connect(2)/sendto(2)/recvfrom(2) syscalls
 * - **UEFI:** EFI_TCP4_PROTOCOL / EFI_TCP6_PROTOCOL via Service Binding
 *
 * @par Example Usage:
 * @code
 * auto ipResult = IPAddress::FromString("93.184.216.34");
 * if (!ipResult) return;
 * auto createResult = Socket::Create(ipResult.Value(), 443);
 * if (!createResult) return;
 * Socket &sock = createResult.Value();
 *
 * auto openResult = sock.Open();
 * if (!openResult) return;
 *
 * CHAR request[] = "GET / HTTP/1.1\r\n\r\n";
 * (void)sock.Write(Span<const CHAR>(request, sizeof(request) - 1));
 *
 * CHAR response[4096];
 * auto readResult = sock.Read(Span<CHAR>(response));
 * if (readResult) {
 *     SSIZE bytesRead = readResult.Value();
 * }
 * @endcode
 *
 * @see RFC 9293 — Transmission Control Protocol (TCP)
 *      https://datatracker.ietf.org/doc/html/rfc9293
 * @see RFC 9293 Section 3.5 — Establishing a Connection (three-way handshake)
 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.5
 * @see RFC 9293 Section 3.6 — Closing a Connection
 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.6
 * @see RFC 9293 Section 3.8 — Data Communication
 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.8
 */
class Socket
{
private:
	IPAddress ip;      ///< Remote IP address for this connection
	UINT16 port;       ///< Remote port number in host byte order
	PVOID handle;      ///< Platform-specific socket handle (fd on POSIX, HANDLE on Windows, UefiSocketContext* on UEFI)

	/**
	 * @brief Binds the socket to a local address
	 *
	 * @details Assigns a local address to the socket. On Windows, this is
	 * required before connect() as the AFD driver needs an explicit bind to
	 * a wildcard address. On Linux/macOS the kernel performs an implicit bind
	 * during connect(), so this is only called internally when needed.
	 *
	 * @param socketAddress Local address to bind to
	 * @param shareType Address sharing flags (Windows AFD_SHARE_* flags; ignored on POSIX)
	 * @return Result<void, Error> — Ok on success, Err on bind failure
	 *
	 * @see RFC 9293 Section 3.1 — Header Format (local port assignment)
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.1
	 */
	[[nodiscard]] Result<VOID, Error> Bind(const SockAddr &socketAddress, INT32 shareType);

	/**
	 * @brief Private constructor for factory use
	 * @param ipAddress Remote IP address
	 * @param portNum Remote port number
	 */
	Socket(const IPAddress &ipAddress, UINT16 portNum) : ip(ipAddress), port(portNum), handle(nullptr) {}

public:
	/// @name Stack-Only Enforcement
	/// @{
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; } ///< Placement new for Result<Socket, Error>
	VOID operator delete(VOID *, PVOID) noexcept {}              ///< Matching placement delete
	/// @}

	/**
	 * @brief Default constructor — creates an invalid (unconnected) socket
	 */
	Socket() : ip(), port(0), handle(nullptr) {}

	/**
	 * @brief Creates a TCP stream socket for the specified remote endpoint
	 *
	 * @details Allocates a platform-specific socket handle configured for
	 * TCP (SOCK_STREAM, IPPROTO_TCP) communication. The socket is created
	 * in the CLOSED state per RFC 9293 Section 3.3.2 and must be connected
	 * via Open() before data transfer.
	 *
	 * Platform behavior:
	 * - **Windows:** Opens \\Device\\Afd\\Endpoint via ZwCreateFile with
	 *   AfdOpenPacketXX extended attributes specifying AF_INET/AF_INET6,
	 *   SOCK_STREAM, and IPPROTO_TCP
	 * - **Linux:** Calls socket(domain, SOCK_STREAM, IPPROTO_TCP) syscall
	 * - **macOS:** Calls socket(domain, SOCK_STREAM, IPPROTO_TCP) syscall
	 * - **UEFI:** Locates TCP4/TCP6 Service Binding Protocol, creates a
	 *   child handle, and opens the TCP protocol interface
	 *
	 * @param ipAddress Remote IP address (IPv4 or IPv6)
	 * @param port Remote port number in host byte order
	 * @return Result<Socket, Error> — Ok with the socket on success,
	 *         Err(Socket_CreateFailed_Open) on failure
	 *
	 * @see RFC 9293 Section 3.3.2 — State Machine Overview (CLOSED state)
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.3.2
	 */
	[[nodiscard]] static Result<Socket, Error> Create(const IPAddress &ipAddress, UINT16 port);

	/**
	 * @brief Destructor — closes the socket if still valid
	 */
	~Socket()
	{
		if (IsValid())
			(VOID)Close();
	}

	/// @name Non-Copyable
	/// @{
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;
	/// @}

	/// @name Move Semantics
	/// @{

	/**
	 * @brief Move constructor — transfers socket ownership
	 * @param other Source socket (invalidated after move)
	 */
	Socket(Socket &&other) noexcept : ip(other.ip), port(other.port), handle(other.handle)
	{
		other.handle = nullptr;
	}

	/**
	 * @brief Move assignment — transfers socket ownership, closes existing
	 * @param other Source socket (invalidated after move)
	 * @return Reference to this socket
	 */
	Socket &operator=(Socket &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				(VOID)Close();
			ip = other.ip;
			port = other.port;
			handle = other.handle;
			other.handle = nullptr;
		}
		return *this;
	}
	/// @}

	/**
	 * @brief Checks whether the socket holds a valid OS handle
	 * @return true if the socket has a non-null handle, false otherwise
	 */
	constexpr BOOL IsValid() const { return handle != nullptr; }

	/**
	 * @brief Returns the raw file descriptor / handle value
	 * @return Platform-specific socket descriptor cast to SSIZE
	 */
	SSIZE GetFd() const { return (SSIZE)handle; }

	/**
	 * @brief Connects the socket to the remote endpoint
	 *
	 * @details Initiates the TCP three-way handshake (SYN → SYN-ACK → ACK)
	 * as defined in RFC 9293 Section 3.5, transitioning from CLOSED to
	 * ESTABLISHED state.
	 *
	 * The connection attempt uses a 5-second timeout. If the handshake does
	 * not complete within this window, the operation fails.
	 *
	 * Platform behavior:
	 * - **Windows:** Binds to a wildcard local address via IOCTL_AFD_BIND,
	 *   then connects via IOCTL_AFD_CONNECT with a 5-second timeout using
	 *   ZwWaitForSingleObject
	 * - **Linux:** Sets O_NONBLOCK via fcntl(F_SETFL), calls connect(2),
	 *   waits for POLLOUT via ppoll(2) with 5-second timeout, checks
	 *   SO_ERROR, then restores blocking mode
	 * - **macOS:** Sets O_NONBLOCK via fcntl(F_SETFL), calls connect(2),
	 *   waits for POLLOUT via poll(2) with 5-second timeout, checks
	 *   SO_ERROR, then restores blocking mode
	 * - **UEFI:** Configures TCP4/TCP6 with remote address and port,
	 *   issues Connect() and polls for completion with 5-second timeout
	 *
	 * @return Result<void, Error> — Ok on successful connection,
	 *         Err(Socket_OpenFailed_Connect) on handshake failure or timeout,
	 *         Err(Socket_OpenFailed_EventCreate) if event creation fails (Windows/UEFI)
	 *
	 * @see RFC 9293 Section 3.5 — Establishing a Connection
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.5
	 */
	[[nodiscard]] Result<VOID, Error> Open();

	/**
	 * @brief Closes the socket and releases all associated resources
	 *
	 * @details Releases the platform socket handle. On UEFI, this also
	 * performs a graceful TCP close (sending FIN) before destroying the
	 * protocol child instance and freeing the socket context.
	 *
	 * After Close(), the socket is invalid and must not be used for I/O.
	 * The destructor calls this automatically if the socket is still valid.
	 *
	 * Platform behavior:
	 * - **Windows:** Calls ZwClose() on the AFD handle
	 * - **Linux:** Calls close(2) syscall on the file descriptor
	 * - **macOS:** Calls close(2) syscall on the file descriptor
	 * - **UEFI:** Cancels pending I/O, performs TCP close with abort flag,
	 *   unconfigures the protocol, destroys the child handle, closes the
	 *   Service Binding protocol, and frees the socket context
	 *
	 * @return Result<void, Error> — Ok on success,
	 *         Err(Socket_CloseFailed_Close) on failure (Windows only)
	 *
	 * @see RFC 9293 Section 3.6 — Closing a Connection
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.6
	 */
	[[nodiscard]] Result<VOID, Error> Close();

	/**
	 * @brief Reads data from the connected socket
	 *
	 * @details Receives up to buffer.Size() bytes from the TCP connection.
	 * This is a blocking call that waits for data to arrive. A return value
	 * of 0 indicates the remote peer has closed the connection (received FIN,
	 * per RFC 9293 Section 3.6).
	 *
	 * Platform behavior:
	 * - **Windows:** Issues IOCTL_AFD_RECV via ZwDeviceIoControlFile with
	 *   a 5-minute timeout
	 * - **Linux:** Calls recvfrom(2) syscall (or recv via socketcall on i386)
	 * - **macOS:** Calls recvfrom(2) syscall
	 * - **UEFI:** Issues TCP4/TCP6 Receive() and polls for completion with
	 *   a 60-second timeout
	 *
	 * @param buffer Output buffer to receive data into
	 * @return Result<SSIZE, Error> — Ok with bytes read (0 = connection closed by peer),
	 *         Err(Socket_ReadFailed_Recv) on receive failure,
	 *         Err(Socket_ReadFailed_Timeout) on timeout (Windows only),
	 *         Err(Socket_ReadFailed_EventCreate) if event creation fails (Windows/UEFI)
	 *
	 * @see RFC 9293 Section 3.8 — Data Communication
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.8
	 */
	[[nodiscard]] Result<SSIZE, Error> Read(Span<CHAR> buffer);

	/**
	 * @brief Writes data to the connected socket
	 *
	 * @details Sends buffer.Size() bytes over the TCP connection. The
	 * implementation loops internally until all bytes are sent, handling
	 * partial writes transparently. This ensures the caller does not need
	 * to retry on short sends.
	 *
	 * Platform behavior:
	 * - **Windows:** Issues IOCTL_AFD_SEND via ZwDeviceIoControlFile in a
	 *   loop until all bytes are sent, with a 1-minute per-chunk timeout
	 * - **Linux:** Calls sendto(2) syscall in a loop (or send via
	 *   socketcall on i386)
	 * - **macOS:** Calls sendto(2) syscall in a loop
	 * - **UEFI:** Issues TCP4/TCP6 Transmit() with PSH flag and polls for
	 *   completion with a 30-second timeout
	 *
	 * @param buffer Data to send
	 * @return Result<UINT32, Error> — Ok with total bytes sent,
	 *         Err(Socket_WriteFailed_Send) on send failure,
	 *         Err(Socket_WriteFailed_Timeout) on timeout (Windows only),
	 *         Err(Socket_WriteFailed_EventCreate) if event creation fails (Windows/UEFI)
	 *
	 * @see RFC 9293 Section 3.8 — Data Communication
	 *      https://datatracker.ietf.org/doc/html/rfc9293#section-3.8
	 */
	[[nodiscard]] Result<UINT32, Error> Write(Span<const CHAR> buffer);
};

/** @} */ // end of socket group