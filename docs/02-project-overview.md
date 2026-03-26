# Project Overview and Architecture

What this project actually is, how it's structured, and why the design decisions were made.

---

## What Is an "Agent"?

If you're outside the security world, "agent" might conjure images of AI chatbots or help desk software. In security and penetration testing, an agent (also called an implant or beacon) is software that runs on a target machine and communicates back to a controller. It receives commands, executes them, and sends back results.

This is standard architecture for penetration testing tools. Cobalt Strike has its beacon, Metasploit has meterpreter, Sliver has its implant. Same concept, different implementations. The distinguishing feature of THIS agent is that it compiles to position-independent shellcode — raw machine code that can be loaded and executed from any memory address without any OS loader support.

---

## The Layered Architecture

The codebase follows a strict 4-layer design. Each layer depends only on layers below it — never sideways or upward.

```
+---------------------------------------------------------------+
|  BEACON (src/beacon/)                          6 files         |
|  Agent command loop, handlers, shell, screen capture           |
+---------------------------------------------------------------+
|  LIB (src/lib/)                               30 files         |
|  Crypto, TLS 1.3, HTTP, DNS, WebSocket, JPEG, containers      |
+---------------------------------------------------------------+
|  PLATFORM (src/platform/)                     81 files         |
|  Console, filesystem, sockets, screen, memory, system utils    |
|  Implementations in: posix/, windows/, uefi/                   |
+---------------------------------------------------------------+
|  KERNEL (src/platform/kernel/)                81 files         |
|  Raw syscall definitions and OS API wrappers                   |
|  Per-OS: linux/, windows/, macos/, freebsd/, solaris/, uefi/   |
+---------------------------------------------------------------+
|  CORE (src/core/)                             31 files         |
|  Types, memory ops, strings, math, algorithms, compiler RT     |
+---------------------------------------------------------------+
```

**Core** sits at the bottom. Pure C++ with no knowledge of any operating system. Types (`Result`, `Span`, `Error`), memory operations (`memset`, `memcpy`), strings, math, hashing, Base64, binary I/O. Everything here works identically on every platform.

**Kernel** wraps the raw OS interface. Syscall numbers, inline assembly for `System::Call`, Windows PEB walking and PE export parsing, UEFI protocol definitions. This is where platform-specific details first appear.

**Platform** builds portable abstractions on top of the kernel layer. `File::Create()` works the same whether you're on Windows, Linux, or UEFI. `Socket::Connect()` hides the difference between BSD sockets, Windows AFD IOCTLs, and UEFI TCP protocols. Each subsystem (console, filesystem, sockets, screen, memory, system utils) has a header declaring the interface and per-platform implementations in subdirectories.

**Lib** contains higher-level functionality built on Platform. The full crypto suite (ChaCha20-Poly1305, SHA-256/384, ECC), TLS 1.3 client, HTTP/1.1 client, DNS-over-HTTPS resolver, WebSocket client, JPEG encoder, and a dynamic array container. This layer has zero platform-specific code — it uses the Platform API exclusively.

**Beacon** is the top layer. The actual agent logic: connect to the relay via WebSocket, receive commands, dispatch to handlers, send responses, reconnect on failure. Six files total.

Why strict layering? Because you can swap the Platform layer to port to a new OS without touching Lib or Beacon. That's how 8 platforms are supported without 8 copies of the networking code.

---

## Why So Many Platforms and Architectures?

8 platforms times 7 architectures. That sounds excessive until you think about real-world targets:

- Windows servers and workstations (x86_64, i386)
- Linux containers and servers (x86_64, aarch64, riscv64)
- macOS laptops (x86_64, aarch64)
- Android phones (aarch64, armv7a, x86_64)
- iOS devices (aarch64)
- FreeBSD servers and firewalls (x86_64, aarch64, riscv64)
- Solaris/illumos (x86_64, aarch64)
- UEFI firmware — runs before ANY OS loads (x86_64, aarch64)
- IoT devices on ARM, RISC-V, MIPS

A single codebase that compiles to all of these is extremely valuable for security research. Each combination brings unique challenges: different syscall numbers, calling conventions, binary formats, and platform quirks. The build system (see `docs/03-build-system.md`) manages the combinatorial explosion.

---

## The Relay / C2 Architecture

The agent doesn't connect directly to the operator's machine. It talks to a relay — a Cloudflare Worker at `relay.nostdlib.workers.dev`. The operator also connects to the relay. Commands and responses get forwarded between them through this intermediary.

```
+----------+        HTTPS/WSS        +---------+        HTTPS/WSS       +----------+
|  Agent   | <--------------------> |  Relay  | <--------------------> | Operator |
| (target) |                        | (CF     |                        |  (you)   |
|          |                        | Worker) |                        |          |
+----------+                        +---------+                        +----------+
```

This is standard C2 (command-and-control) architecture. The operator's real IP stays hidden behind the CDN. From a network perspective, the agent's traffic looks like normal HTTPS to a Cloudflare domain — indistinguishable from legitimate web traffic.

The connection chain from the agent's perspective:
1. DNS-over-HTTPS resolves `relay.nostdlib.workers.dev`
2. TCP socket connects to the resolved IP on port 443
3. TLS 1.3 handshake establishes encryption
4. HTTP/1.1 request with `Upgrade: websocket`
5. WebSocket binary frames carry commands and responses

See `docs/12-networking.md` for the full breakdown of each layer.

---

## The 8 Command Types

The agent supports 8 commands, dispatched via a function pointer array (see `docs/13-beacon.md`):

| Command | What It Does |
|---------|-------------|
| `GetSystemInfo` | Collects hostname, OS version, machine UUID, CPU architecture |
| `GetDirectoryContent` | Lists files and folders in a directory (like `ls` or `dir`) |
| `GetFileContent` | Reads a file at a specified offset |
| `GetFileChunkHash` | SHA-256 hash of a file chunk (for integrity checking) |
| `WriteShell` | Sends keyboard input to an interactive shell process |
| `ReadShell` | Reads output from the shell process |
| `GetDisplays` | Enumerates connected monitors with geometry info |
| `GetScreenshot` | Captures the screen as JPEG with incremental dirty-rectangle compression |

These are the fundamental building blocks an operator needs during an engagement. Nothing flashy — just solid primitives that compose well.

---

## What "Zero Dependencies" Actually Means

The README claims "zero dependencies." This needs unpacking because it's a nuanced claim.

**Runtime dependencies: truly zero.** No libc, no C++ standard library, no third-party libraries linked in. The binary is a single `.text` section of raw machine code. Everything the agent needs — crypto (SHA-256, ChaCha20, ECC), TLS 1.3, HTTP, DNS, WebSocket, JPEG encoding — is implemented from scratch.

**Build-time dependencies: several.** You need LLVM 22+ (for the pic-transform compiler pass), CMake 3.20+, Ninja, and the platform-appropriate toolchain. These are not runtime dependencies — they're tools used during compilation.

**OS dependency: the kernel.** The code still needs an operating system kernel underneath to handle syscalls. It doesn't need the C library, but it does need the kernel. On UEFI, it needs the firmware's boot services.

The real distinction is between runtime and build-time dependencies. At runtime, the shellcode is completely self-contained. That's what makes it position-independent.

---

## Common Problems and Solutions

The "Common Problems and Solutions" section in `README.md` (lines 216-284) is actually the most educational part of the entire project documentation. Each "problem" is really a lesson in how compilers and linkers work:

**"String literals land in .rodata"** — When you write `const char* msg = "Hello"`, the compiler stores "Hello" as a constant in the `.rodata` section. This creates a data section, violating the Golden Rule. The pic-transform LLVM pass rewrites these as stack-based immediate stores. See `docs/14-pic-transform.md`.

**"Global variables create .data/.bss"** — Any `static` or global variable creates a `.data` or `.bss` section. The project forbids globals entirely. State is passed on the stack or stored in CPU registers.

**"Compiler optimizations move stack data to .rodata"** — Even if you carefully put data on the stack, aggressive compiler optimizations can recognize that the values are constants and hoist them back into `.rodata`. The `optnone` attribute and register barriers prevent this. This is one of the most insidious issues because the source code looks correct but the compiled output has data sections.

**"Function pointers create GOT/PLT entries"** — Taking the address of a function can create entries in the Global Offset Table (GOT) or Procedure Linkage Table (PLT), which are data sections used by dynamic linkers. Hidden visibility (`-fvisibility=hidden`) and direct calls prevent this.

Each of these problems is deep enough to be its own chapter. They're the "war stories" of the project — lessons learned through painful debugging. See `docs/03-build-system.md` for how the build system defends against all of them.

---

## Next Steps

- `docs/03-build-system.md` — How the build pipeline turns C++23 into raw shellcode
- `docs/04-entry-point.md` — Where execution begins
- `docs/05-core-types.md` — The vocabulary types used everywhere
