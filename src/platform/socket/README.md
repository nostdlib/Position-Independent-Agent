[< Back to Platform](../README.md) | [< Back to Project Root](../../../README.md)

# TCP Socket Networking

Platform-independent TCP stream sockets supporting IPv4 and IPv6. Three fundamentally different networking interfaces: POSIX sockets, the Windows AFD driver, and UEFI TCP protocols.

## Windows: Bypassing Winsock via the AFD Driver

Instead of the Winsock2 API (`WSAStartup`, `WSASocket`, etc.), the runtime talks directly to the **Ancillary Function Driver (AFD)** — the kernel-mode driver that Winsock itself calls internally.

### How AFD Sockets Work

A socket is opened as a **file** on the AFD device:

```c
ZwCreateFile(&handle,
    path = L"\\Device\\Afd\\Endpoint",
    EaBuffer = AfdSocketParams {
        AfdOperation = "AfdOpenPacketXX",   // magic EA name
        AddressFamily = AF_INET,
        SocketType = SOCK_STREAM,
        Protocol = IPPROTO_TCP
    })
```

The Extended Attributes (EA) buffer encodes the socket parameters as a named structure — `"AfdOpenPacketXX"` tells the AFD driver to create a TCP socket endpoint.

### AFD IOCTL Encoding

All socket operations use `ZwDeviceIoControlFile` with IOCTLs encoded as:

```c
IOCTL = (DeviceType << 12) | (FunctionCode << 2) | Method
//       0x12 (AFD)          0-7                    3 (neither)

IOCTL_AFD_BIND    = (0x12 << 12) | (0 << 2) | 3  // = 0x12003
IOCTL_AFD_CONNECT = (0x12 << 12) | (1 << 2) | 3  // = 0x12007
IOCTL_AFD_RECV    = (0x12 << 12) | (5 << 2) | 3  // = 0x12017
IOCTL_AFD_SEND    = (0x12 << 12) | (7 << 2) | 3  // = 0x1201F
```

### Connect with Timeout

AFD connect is asynchronous — the runtime manages the async lifecycle manually:

```
1. ZwCreateEvent(&event)                          → create synchronization event
2. ZwDeviceIoControlFile(handle, event,           → issue async connect
     IOCTL_AFD_BIND, &bindData)                    (must bind to wildcard first)
3. ZwDeviceIoControlFile(handle, event,
     IOCTL_AFD_CONNECT, &connectInfo)
4. ZwWaitForSingleObject(event, &timeout)         → wait up to 5 seconds
     │
     ├─ STATUS_SUCCESS  → connected
     ├─ STATUS_TIMEOUT  → connection timed out
     └─ STATUS_PENDING  → still in progress (shouldn't happen after wait)
```

Timeout values use Windows' 100-nanosecond units, with negative values meaning "relative from now":
```c
LARGE_INTEGER timeout;
timeout.QuadPart = -5LL * 1000LL * 10000LL;  // -50,000,000 = 5 seconds
```

### Send/Receive Indirection

AFD uses a two-level buffer descriptor:

```
AfdSendRecvInfo {
    BufferArray → AfdWsaBuf[] {
        { Length = 1024, Buffer = dataPtr }    // scatter/gather array
    }
    BufferCount = 1
    AfdFlags, TdiFlags
}
```

This mirrors Winsock's `WSABUF` scatter/gather pattern, but at the driver level.

## POSIX: Non-Blocking Connect with Poll

POSIX sockets use the standard non-blocking connect pattern:

```
1. socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) → fd
2. fcntl(fd, F_SETFL, O_NONBLOCK)            → make non-blocking
3. connect(fd, addr, len)                     → returns -EINPROGRESS immediately
4. ppoll/poll(fd, POLLOUT, 5 seconds)         → wait for connection
5. getsockopt(fd, SOL_SOCKET, SO_ERROR, &err) → check actual result
6. fcntl(fd, F_SETFL, ~O_NONBLOCK)            → back to blocking
```

### Linux i386 socketcall Multiplexer

i386 Linux lacks direct socket syscalls. Instead, all socket operations go through a single `SYS_SOCKETCALL` (102) multiplexer:

```c
// To create a socket on i386:
USIZE args[] = { domain, type, protocol };
System::Call(SYS_SOCKETCALL, SOCKOP_SOCKET, (USIZE)args);

// To connect:
USIZE args[] = { fd, (USIZE)addr, addrlen };
System::Call(SYS_SOCKETCALL, SOCKOP_CONNECT, (USIZE)args);
```

The kernel unpacks the argument array based on the operation code. All other architectures have direct syscalls (`SYS_SOCKET`, `SYS_CONNECT`, etc.).

### Platform-Specific Polling

| Platform | Syscall | Timeout Format |
|---|---|---|
| Linux / Android | `SYS_PPOLL` | `Timespec` (nanosecond) |
| Solaris | `SYS_POLLSYS` | `Timespec` (nanosecond) |
| macOS / iOS / FreeBSD | `SYS_POLL` | Milliseconds (integer) |

### BSD sockaddr Length Field

BSD-derived systems (macOS, iOS, FreeBSD) require a `sin_len` field at the start of `sockaddr`:

```c
#if defined(BSD)
struct SockAddr {
    UINT8 SinLen;      // Structure length (BSD-specific)
    UINT8 SinFamily;   // AF_INET (fits in 1 byte on BSD)
    ...
};
#else
struct SockAddr {
    UINT16 SinFamily;  // AF_INET (16-bit on Linux/Windows)
    ...
};
#endif
```

### AF_INET6 Value Divergence

Every platform family has a different value for `AF_INET6`:

| Platform | AF_INET6 |
|---|---|
| Linux | 10 |
| Windows / UEFI | 23 |
| macOS / iOS | 30 |
| FreeBSD | 28 |
| Solaris | 26 |

## UEFI: Protocol-Based TCP Stack

UEFI networking uses firmware-provided TCP protocol interfaces, not syscalls:

```
1. LocateProtocol(TCP4_SERVICE_BINDING_GUID)     → service binding
2. ServiceBinding→CreateChild(&childHandle)       → per-connection handle
3. HandleProtocol(child, TCP4_PROTOCOL_GUID)      → tcp protocol
4. tcp→Configure(AccessPoint: { local, remote })  → set addresses/ports
5. tcp→Connect(&token)                            → async connect
6. Poll for completion with Stall(1ms) loop       → pseudo-blocking wait
```

UEFI has no true async mechanism (no signals, no poll syscall). The runtime busy-polls with `Stall(1000)` (1ms sleep) between checks:

```c
while (token.Status == EFI_NOT_READY) {
    tcp->Poll(tcp);           // kick the firmware
    bs->Stall(1000);          // sleep 1ms
    if (elapsed > timeout)    // manual timeout check
        return EFI_TIMEOUT;
}
```

### GUID Stack Construction

Protocol GUIDs cannot live in `.rdata` (position-independent requirement). They're constructed field-by-field at runtime:

```c
EFI_GUID tcpGuid;
tcpGuid.Data1 = 0x00720665;
tcpGuid.Data2 = 0x67EB;
tcpGuid.Data3 = 0x4A99;
// ...
```

### Network Initialization Sequence

UEFI networking requires explicit stack initialization:
1. Start Simple Network Protocol (SNP)
2. Initialize the NIC
3. Configure DHCP (IP4 Config2 with `Dhcp` policy)
4. Wait for DHCP completion (50 retries × 100ms = 5s timeout)
5. 500ms settling delay for TCP stack readiness
6. Create TCP child handle

The state flags `NetworkInitialized`, `DhcpConfigured`, `TcpStackReady` in `EFI_CONTEXT` prevent re-initialization.
