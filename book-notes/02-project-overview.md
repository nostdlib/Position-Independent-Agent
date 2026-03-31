# 02 - Project Overview (README and Architecture)

Notes on the top-level README.md and overall project structure. These entries
cover the "what" and "why" before we get into the "how."

---

### What is an "agent" in this context?
**File:** `README.md` **Line(s):** 1-10
**Type:** QUESTION
**Priority:** HIGH

The project is called "Position-Independent Agent." Readers outside the security
world will hear "agent" and think AI chatbot or something from a help desk.

In security/C2 terminology, an "agent" (also called an "implant" or "beacon")
is software that runs on a target machine and communicates back to a controller.
It receives commands, executes them, and sends back results. This is standard
architecture for penetration testing tools — think Cobalt Strike's beacon,
Metasploit's meterpreter, or Sliver's implant. Same concept, different
implementation.

---

### What does the 4-layer architecture actually mean?
**File:** `src/README.md` **Line(s):** 8-27
**Type:** QUESTION
**Priority:** HIGH

The architecture diagram shows Core -> Platform -> Lib -> Beacon. The key rule
is that each layer can only depend on layers below it, never above.

**Core** sits at the bottom: pure C++ with no OS knowledge. Types, math,
strings, memory primitives. **Platform** wraps OS-specific syscalls into a
unified API — "write to a file" works the same whether you're on Windows or
Linux. **Lib** builds higher-level functionality on top of Platform: TLS, HTTP,
DNS, crypto. **Beacon** is the top layer — the actual agent logic with command
handling and the main loop.

Why bother with strict layering? Because you can swap the Platform layer to port
to a new OS without touching Lib or Beacon. That's how the project supports 8
platforms without 8 copies of the networking code.

---

### Why does the README list so many platforms and architectures?
**File:** `README.md` **Line(s):** 41
**Type:** QUESTION
**Priority:** MEDIUM

"8 platforms x 7 architectures." That sounds excessive until you think about
real-world targets: Windows servers, Linux containers, macOS laptops, Android
phones, IoT devices running ARM or RISC-V, network appliances on MIPS. A single
codebase that compiles to all of these is extremely valuable for security
research. Each platform/architecture combination brings unique challenges —
different syscall numbers, calling conventions, binary formats — and the project
handles all of them.

---

### What is the "relay" mentioned in the architecture?
**File:** `README.md` **Line(s):** 95-127
**Type:** QUESTION
**Priority:** MEDIUM

The agent doesn't connect directly to the operator's machine. Instead it talks
to a relay — in this case a Cloudflare Worker at `relay.nostdlib.workers.dev`.
The operator also connects to the relay. Commands and responses get forwarded
between them through this intermediary.

This is standard C2 architecture: agent -> relay -> operator. The operator's
real IP stays hidden, and from a network perspective the traffic looks like
normal HTTPS to a CDN. Nothing exotic about the pattern; the implementation
details are what make it interesting.

---

### What are the 9 command types?
**File:** `README.md` **Line(s):** 42-50
**Type:** QUESTION
**Priority:** MEDIUM

The README lists these but doesn't say much about what each one actually does:

- **GetSystemInfo** — hostname, OS version, machine ID, etc.
- **GetDirectoryContent** — lists files and folders (like `ls` or `dir`)
- **GetFileContent** — downloads a file from the target
- **WriteShell** — sends keyboard input to an interactive shell
- **ReadShell** — reads output from that shell
- **GetDisplays** — enumerates connected monitors
- **GetScreenshot** — captures what's on screen, with incremental dirty-rect compression

These are the fundamental operations an operator needs during an engagement.
Nothing flashy — just solid building blocks.

---

### What does "zero dependencies" really mean?
**File:** `README.md` **Line(s):** 39
**Type:** COMMENT
**Priority:** HIGH

"Zero dependencies" is a nuanced claim that needs unpacking.

What it DOES mean: no libc, no C++ standard library, no third-party libraries
linked in. All crypto, networking, and image encoding are implemented from
scratch. What it DOES NOT mean: no OS. The code still needs a kernel underneath
to handle syscalls. It also doesn't mean no tooling — you need LLVM 22+ with
the pic-transform pass, CMake, and Ninja to build it.

The real distinction is between runtime dependencies (truly zero) and build-time
dependencies (there are several). That difference matters and the text should be
precise about it.

---

### What is the "Common Problems and Solutions" section really about?
**File:** `README.md` **Line(s):** 216-284
**Type:** COMMENT
**Priority:** HIGH

This section is actually the most educational part of the entire README. Each
"problem" is really a lesson in how compilers and linkers work under the hood:

"String literals land in .rodata" teaches how compilers handle constants.
"Global variables create .data/.bss" explains what global state is and why it's
forbidden here. "Compiler optimizations move stack data to .rodata" shows how
aggressive optimizers can defeat your PIC intentions — this one's gonna trip
people up. "Function pointers create GOT/PLT entries" gets into what dynamic
linking looks like at the machine level.

Each of these is deep enough to be its own chapter. They're the "war stories"
of the project, and they'll resonate with readers who've fought similar battles.
