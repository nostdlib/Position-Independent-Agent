# Platform Abstraction: One API, Eight Operating Systems

This document covers the platform abstraction layer -- the wrappers that let the
codebase perform I/O, allocate memory, open sockets, capture screens, and spawn
processes without importing a single library function. Every operation goes through
direct syscalls or firmware protocol calls. No libc, no CRT, no Winsock, no libX11.

Source files: `src/platform/console/console.h`, `src/platform/fs/file.h`,
`src/platform/memory/allocator.h`, `src/platform/memory/posix/memory.cc`,
`src/platform/socket/socket.h`, `src/platform/socket/posix/socket.cc`,
`src/platform/screen/posix/screen.cc`, `src/platform/system/posix/pty.cc`

See also: [01-what-is-pic.md](01-what-is-pic.md) for position-independence constraints,
[05-core-types.md](05-core-types.md) for `Result`/`Span`/`Error`, and
[06-memory-and-strings.md](06-memory-and-strings.md) for custom `memset`/`memcpy`.

---

## 1. Console Without printf: The Full Call Trace

The console is a static class. No instance, no vtable, no global state. Here is
the exact path `Console::WriteFormatted("Hello %d", 42)` takes:

```
Console::WriteFormatted("Hello %d", 42)
  -> StringFormatter::Format(&FormatterCallback<CHAR>, nullptr, "Hello %d", Arg(42))
    -> callback('H'), callback('e'), callback('l'), ...
      -> Console::Write(Span<const CHAR>(&ch, 1))
        -> POSIX:   System::Call(SYS_WRITE, 1, data, size)     // fd 1 = stdout
        -> Windows: NTDLL::ZwWriteFile(consoleHandle, ...)
        -> UEFI:    SystemTable->ConOut->OutputString(data)
```

Each character from the formatter invokes a callback that issues a syscall. No
buffering, no heap allocation, no FILE* streams. The callback is minimal:

```cpp
template <TCHAR TChar>
BOOL Console::FormatterCallback([[maybe_unused]] PVOID context, TChar ch)
{
    return Write(Span<const TChar>(&ch, 1));
}
```

And `WriteFormatted` forwards directly to `StringFormatter`:

```cpp
template <TCHAR TChar, typename... Args>
UINT32 Console::WriteFormatted(const TChar *format, Args &&...args)
{
    return StringFormatter::Format(
        &FormatterCallback<TChar>, nullptr, format, static_cast<Args &&>(args)...);
}
```

The variadic template pack replaces `va_list` -- the compiler knows every argument
type at compile time. The `&FormatterCallback<TChar>` function pointer is transformed
to a PC-relative reference by the LLVM pic-transform pass, keeping the code
position-independent (see [01-what-is-pic.md](01-what-is-pic.md)).

---

## 2. File I/O Without fopen

`File::Open` is a factory returning `Result<File, Error>`. Underneath, it calls
the native API per platform: `open()`/`openat()` on POSIX, `ZwCreateFile()` on
Windows, `EFI_FILE_PROTOCOL->Open()` on UEFI.

The object stores two fields: a platform handle and a cached file size. Mode flags
are bitmask constants (`ModeRead`, `ModeWrite`, `ModeAppend`, `ModeCreate`,
`ModeTruncate`, `ModeBinary`). Read and Write return `Result<UINT32, Error>`.

On POSIX, fd 0 is valid (stdin), so `nullptr` cannot be the invalid sentinel:

```cpp
static FORCE_INLINE PVOID InvalidFileHandle()
{
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS) || ...
    return (PVOID)(SSIZE)-1;   // -1 cast to pointer
#else
    return nullptr;             // Windows uses nullptr
#endif
}
```

---

## 3. RAII: Why Every Resource Uses It

Acquire the resource in the constructor, release it in the destructor. The compiler
guarantees the destructor runs when the object leaves scope:

```cpp
{
    auto result = File::Open(L"test.txt", File::ModeRead);
    if (result.IsOk()) {
        auto file = result.Value();
        file.Read(...);
    }  // destructor calls Close() here
}
```

Three enforcement mechanisms make this airtight. Copy is deleted (prevents
double-close). Move transfers ownership (source handle becomes invalid). Heap
`new`/`delete` are deleted (prevents escaping scope). The same pattern applies
to `Socket`, `Pty`, `Process`, and every other OS resource wrapper.

---

## 4. Memory Allocation Without malloc

The `Allocator` class goes directly to the OS virtual memory API:

| Platform | Allocate | Release |
|---|---|---|
| POSIX | `mmap(PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS)` | `munmap(ptr, size)` |
| Windows | `NtAllocateVirtualMemory` | `NtFreeVirtualMemory` |
| UEFI | `BootServices->AllocatePool()` | `BootServices->FreePool()` |

Global `operator new` and `operator delete` are overridden to route through it:

```cpp
PVOID operator new(USIZE size)          { return Allocator::AllocateMemory(size); }
VOID operator delete(PVOID p) noexcept  { Allocator::ReleaseMemory(p, 0); }
```

Every allocation burns at least one 4KB page. Wasteful for small objects, but
dead simple with zero dependencies.

### The POSIX Size Header Trick

`munmap()` requires both pointer and size, but `operator delete(void*)` only
provides the pointer. The fix: prepend the size to every allocation.

```cpp
PVOID Allocator::AllocateMemory(USIZE size)
{
    USIZE totalSize = (size + sizeof(USIZE) + 4095) & ~(USIZE)4095;
    SSIZE result = System::Call(SYS_MMAP, ...totalSize...);

    PCHAR base = (PCHAR)result;
    *(USIZE*)base = totalSize;             // store size in header
    return (PVOID)(base + sizeof(USIZE));  // return pointer past header
}

VOID Allocator::ReleaseMemory(PVOID address, USIZE)
{
    PCHAR base = (PCHAR)address - sizeof(USIZE);  // back up to header
    USIZE totalSize = *(USIZE*)base;               // read stored size
    System::Call(SYS_MUNMAP, (USIZE)base, totalSize);
}
```

Windows does not need this -- `NtFreeVirtualMemory` frees the entire region from
just the base pointer.

### The FreeBSD i386 mmap Hack

FreeBSD's `mmap` on i386 takes 7 arguments (the offset is 64-bit `off_t`, split
across two 32-bit stack slots). `System::Call` only supports 6, so raw inline
assembly pushes all 8 stack slots directly:

```asm
pushl $0          // off_t high = 0
pushl $0          // off_t low = 0
pushl %%edi       // fd = -1
pushl %%esi       // flags
pushl %%edx       // prot
pushl %%ecx       // len
pushl %%ebx       // addr
pushl $0          // dummy return address
int $0x80
addl $32, %%esp
```

---

## 5. Sockets: Three-Platform Comparison

| Operation | POSIX | Windows (AFD Driver) | UEFI (Firmware Protocol) |
|---|---|---|---|
| **Create** | `socket()` syscall | `ZwCreateFile("\\Device\\Afd\\Endpoint")` | `LocateProtocol(TCP4_GUID)` |
| **Connect** | `connect()` + `poll`/`ppoll` | `IOCTL_AFD_CONNECT` + `ZwWaitForSingleObject` | Token poll loop with `Stall` |
| **Send** | `write()`/`sendto()` | `IOCTL_AFD_SEND` | `protocol->Transmit()` |
| **Receive** | `read()`/`recvfrom()` | `IOCTL_AFD_RECV` | `protocol->Receive()` |
| **Close** | `close()` syscall | `ZwClose()` | `protocol->Close()` |
| **Timeout** | `ppoll`(ns) / `poll`(ms) | 100ns units, negative=relative | `Stall(1000)` busy-poll |

On Windows, sockets bypass Winsock entirely. The runtime talks to the Ancillary
Function Driver (AFD) -- the kernel-mode driver Winsock calls internally. A socket
is a file handle on `\\Device\\Afd\\Endpoint` with parameters in Extended Attributes.

On UEFI, the firmware provides TCP as a protocol object. Since UEFI has no async
primitives, the runtime busy-polls with `Stall(1000)` and checks a completion token.

### Non-Blocking Connect with Timeout

From `src/platform/socket/posix/socket.cc`:

```cpp
// 1. Save flags, set non-blocking
SSIZE flags = PosixFcntl(sockfd, F_GETFL);
PosixFcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

// 2. connect() returns immediately with -EINPROGRESS
SSIZE result = PosixConnect(sockfd, &addrBuffer, addrLen);

// 3. Poll for writability with 5-second timeout
if (result != 0 && (-(INT32)result) == EINPROGRESS) {
    Pollfd pfd = { .Fd = (INT32)sockfd, .Events = POLLOUT };
    SSIZE pollResult = PosixPoll(&pfd, 1, 5);

    // 4. Verify via SO_ERROR
    INT32 sockError = 0;
    PosixGetsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sockError, &optLen);
}

// 5. Restore blocking mode
PosixFcntl(sockfd, F_SETFL, flags);
```

---

## 6. Windows Timeout Units

Four platforms, four timeout conventions:

```
Windows:     timeout.QuadPart = -5LL * 1000LL * 10000LL  // = -50,000,000
             (negative = relative from now, units = 100 nanoseconds)

Linux ppoll: Timespec { .tv_sec = 5, .tv_nsec = 0 }
macOS poll:  timeout_ms = 5000
UEFI:        Stall(5000000)  // microseconds
```

The Windows convention is the most dangerous. Positive values are absolute timestamps
relative to January 1, 1601. Negative values are relative intervals from now. Get the
sign wrong and you either return instantly or wait until the heat death of the universe.

---

## 7. Linux Screen Capture: Three Fallbacks

### Tier 1: X11 Wire Protocol
Parse `~/.Xauthority` for the MIT-MAGIC-COOKIE-1 token, connect to
`/tmp/.X11-unix/X{displayNum}` via Unix socket, send `GetImage` (opcode 73) in
`ZPixmap` format. No `libX11.so` -- speaks the wire protocol directly.

### Tier 2: DRM Dumb Buffers
Open `/dev/dri/card0`, enumerate CRTCs via `DRM_IOCTL_MODE_GETRESOURCES`, map the
active framebuffer via `DRM_IOCTL_MODE_MAP_DUMB`. If the buffer is all-black
(GPU-composited scanout), falls through to Tier 3.

### Tier 3: fbdev
Open `/dev/fb0` through `/dev/fb7`, query resolution via `FBIOGET_VSCREENINFO`,
`mmap` to read pixels. On Android, tries `/dev/graphics/fb0` as a variant. Simplest
method, no multi-monitor support.

---

## 8. macOS Fork-Based Crash Isolation

CoreGraphics is loaded via `dlopen` at runtime. It may crash on headless systems or
sandboxed environments. The runtime forks a sacrificial child:

```
pid = fork()
  +-- Child:  load CoreGraphics, call CGMainDisplayID()
  |           exit(0) on success, exit(1) on failure
  +-- Parent: wait4(pid) and inspect exit status
              Child exited 0   -> safe to use CoreGraphics
              Child crashed/1  -> skip CoreGraphics entirely
```

Position-independent code does not know its environment. It might be on a Mac Mini
with a display or inside a headless container. The fork probe answers the question
without risking the parent process.

---

## 9. PTY Creation: Four Mechanisms

PTY creation is one of the most divergent operations in the codebase:

**Linux:** `open("/dev/ptmx")`, `ioctl(TIOCSPTLCK)` to unlock, `ioctl(TIOCGPTN)` to
get the slave number, manually build `"/dev/pts/{N}"`.

**macOS:** `open("/dev/ptmx")`, `ioctl(TIOCPTYGRANT)` + `ioctl(TIOCPTYUNLK)` to
grant and unlock, `ioctl(TIOCPTYGNAME)` fills the slave path directly.

**FreeBSD:** `posix_openpt()` syscall (auto-unlocks), `ioctl(FIODGNAME)` returns
device name like `"pts/0"`, prepend `"/dev/"`.

**Solaris:** `openat("/dev/ptmx")`, STREAMS ioctls (`I_STR` with `OWNERPT`/`UNLKPT`),
extract slave number from `fstatat()` -> `minor(st_rdev)`.

All four paths share a hidden problem: converting the PTY number to a string path
without `sprintf`. The code does it by hand:

```cpp
char d[16];
USIZE n = 0;
INT32 v = ptyNum;
while (v > 0) {
    d[n++] = '0' + (v % 10);
    v /= 10;
}
while (n > 0)
    slavePath[i++] = d[--n];
```

This is the kind of utility code most projects never think about because `libc`
provides it. Without `libc`, you build it yourself. See
[06-memory-and-strings.md](06-memory-and-strings.md) for more on rebuilding standard
library primitives from scratch.

---

## Summary

The platform layer follows one structural pattern everywhere: a public API with a
factory method returning `Result<T, Error>`, RAII ownership, move-only semantics, and
direct syscalls with no intermediary. Below that clean interface is raw divergence --
different syscall numbers, ioctl encodings, struct layouts, timeout conventions, and
PTY mechanisms. If you understand how `File` works, you understand the shape of
`Socket`, `Pty`, `Screen`, and `Allocator`. The details change. The architecture
does not.
