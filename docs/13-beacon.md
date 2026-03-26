# The Beacon: Agent Command Loop and Handlers

This is where all the lower layers converge into a working agent. The networking
stack, the platform abstraction, the memory primitives, the shell and screen
capture subsystems -- they all exist to serve the code described in this document.
If you have read the preceding material, you already understand each piece in
isolation. Now you see them assembled.

**Prerequisites:** [04 - Entry Point](04-entry-point.md) (how `start()` gets
called), [06 - Memory and Strings](06-memory-and-strings.md) (manual allocation),
[05 - Core Types](05-core-types.md) (the `Span`, `Result`, and primitive
typedefs used everywhere).

**Primary source files:**
- `src/beacon/main.cc` -- the main loop (118 lines)
- `src/beacon/commands.h` -- `CommandType` enum, `Context` struct, handler typedefs
- `src/beacon/commandsHandler.cc` -- all command handler implementations
- `src/beacon/shell.h` -- interactive shell wrapper
- `src/beacon/screen_capture.h` -- screenshot pipeline types

---

## 1. The Main Loop, Step by Step

Open `src/beacon/main.cc`. The `start()` function is the entire agent. It does
three things: build the dispatch table, connect, and process commands. Here is the
full flow, annotated against the source.

**Step 1 -- Build the dispatch table (lines 38-46):**

```cpp
CommandHandler commandHandlers[CommandType::CommandTypeCount] = {nullptr};
commandHandlers[CommandType::Command_GetSystemInfo] = Handle_GetSystemInfoCommand;
commandHandlers[CommandType::Command_GetDirectoryContent] = Handle_GetDirectoryContentCommand;
commandHandlers[CommandType::Command_GetFileContent] = Handle_GetFileContentCommand;
commandHandlers[CommandType::Command_GetFileChunkHash] = Handle_GetFileChunkHashCommand;
commandHandlers[CommandType::Command_WriteShell] = Handle_WriteShellCommand;
commandHandlers[CommandType::Command_ReadShell] = Handle_ReadShellCommand;
commandHandlers[CommandType::Command_GetDisplays] = Handle_GetDisplaysCommand;
commandHandlers[CommandType::Command_GetScreenshot] = Handle_GetScreenshotCommand;
```

This array lives on the stack. That matters -- see section 2.

**Step 2 -- Connect (lines 50-62):**

The outer `while (1)` loop runs forever. Each iteration attempts a WebSocket
connection to the relay:

```cpp
auto createResult = WebSocketClient::Create(url);
```

That single call hides an entire chain: DNS resolution, TCP connect, TLS
handshake, HTTP upgrade, WebSocket handshake. If any step fails, the agent
logs the failure and loops back. There is no backoff in the current code -- it
retries immediately. Each connection attempt is completely stateless. No cached
sessions, no leftover context from the previous attempt.

**Step 3 -- Message loop (lines 65-113):**

The inner `while (1)` reads binary WebSocket frames. Each frame is a command:

```cpp
auto readResult = wsClient.Read();
// ...
PCHAR command = (PCHAR)(readResult.Value().Data);
UINT8 commandType = command[0];     // first byte = command type
command++;                           // rest = payload
USIZE commandLength = readResult.Value().Length - sizeof(UINT8);
```

The first byte is the command type. The remaining bytes are the payload. The
agent indexes into the handler array and calls:

```cpp
commandHandlers[commandType](command, commandLength, &response, &responseLength, &context);
```

After the handler returns, the response is sent back over WebSocket and then
`delete[]`'d. If the read or write fails, the inner loop breaks and the agent
reconnects from scratch.

Two things to internalize: the agent never voluntarily exits, and every
reconnection starts with a blank slate.

---

## 2. Function Pointer Dispatch -- Why Not vtables?

The dispatch table is a plain C array of function pointers:

```cpp
using CommandHandler = VOID (*)(PCHAR command, USIZE commandLength,
                                PPCHAR response, PUSIZE responseLength,
                                Context *context);
```

Why not use a class hierarchy with virtual methods? Because virtual dispatch
requires vtables, and vtables are stored in `.rodata`. This project compiles to
a single `.text` section (see [01 - What Is PIC](01-what-is-pic.md)). Any data
in `.rodata` breaks position independence.

A function pointer array allocated on the stack avoids this entirely. The
pointers themselves resolve to PC-relative addresses at link time (the PIC
transform handles this -- see [14 - PIC Transform](14-pic-transform.md)). The
array is just eight stack slots. No data sections needed.

A `switch/case` would also work, but the array approach is both faster (O(1)
index vs. a jump table the compiler may or may not generate) and easier to
extend. Adding a new command means writing a handler and inserting one line into
the array initialization.

---

## 3. Wire Format and `#pragma pack`

Commands arrive as raw bytes over a binary WebSocket frame. The agent casts
those bytes directly to struct pointers. For that to work, the struct layout
must match the protocol exactly -- no compiler-inserted padding.

From `src/beacon/commandsHandler.cc` (lines 17-31):

```cpp
#pragma pack(push, 1)
struct WireDirectoryEntry
{
    CHAR16 Name[256];
    UINT64 CreationTime;
    UINT64 LastModifiedTime;
    UINT64 Size;
    UINT32 Type;
    BOOL IsDirectory;
    BOOL IsDrive;
    BOOL IsHidden;
    BOOL IsSystem;
    BOOL IsReadOnly;
};
#pragma pack(pop)
```

Without `#pragma pack(push, 1)`, the compiler would insert alignment padding.
A struct with a `UINT8` followed by a `UINT32` normally occupies 8 bytes (3
bytes of padding). Packed, it occupies 5. When the relay sends exactly 5 bytes,
the packed struct matches. The padded struct reads garbage.

One critical detail: wire strings use `CHAR16` (always 2 bytes), never `WCHAR`.
On Windows, `WCHAR` is 2 bytes and this would work by accident. On Linux and
macOS, `WCHAR` is 4 bytes. Using `WCHAR` in a wire struct would silently corrupt
every string field. `CHAR16` is a fixed 2-byte type on all platforms.

The `AgentBuildInfo` struct in `commands.h` follows the same pattern:

```cpp
#pragma pack(push, 1)
struct AgentBuildInfo
{
    UINT32 BuildNumber;
    CHAR CommitHash[9]; // 8 hex chars + null
};
#pragma pack(pop)
```

---

## 4. Handler Memory Ownership

Every handler follows the same calling convention. Look at the typedef again:

```cpp
VOID Handle_*(PCHAR command, USIZE commandLength,
              PPCHAR response, PUSIZE responseLength, Context *context);
```

The `PPCHAR response` is a double pointer -- an out-parameter. The handler
allocates a response buffer and writes through the pointer so the caller
receives it:

```
1. Parse the payload (if any)
2. Gather data (read files, query system info, capture screen, etc.)
3. Allocate: *response = new CHAR[needed_size]
4. Write the status code as the first 4 bytes: *(PUINT32)*response = StatusSuccess
5. Write the payload after the status code
6. Set *responseLength to the total size
7. Return
```

The main loop owns the buffer after the handler returns. It sends the response
over WebSocket and then calls `delete[] response`. There are no RAII wrappers,
no smart pointers, no reference counting. It is explicit manual memory
management everywhere. If a handler throws (it cannot -- exceptions are
disabled via `-fno-exceptions`, see [03 - Build System](03-build-system.md)),
the buffer would leak. Since exceptions are off, that is not a concern.

For unknown commands, the main loop itself allocates a minimal response:

```cpp
response = new CHAR[responseLength];
*(PUINT32)response = StatusCode::StatusUnknownCommand;
```

---

## 5. The Context Struct

Most commands are stateless. `GetSystemInfo` gathers information, writes it,
and is done. `GetFileContent` reads a file and returns it. No state persists
between calls.

But two subsystems need persistent state:

```cpp
struct Context
{
    Shell *shell = nullptr;
    ScreenCaptureContext *screenCaptureContext = nullptr;

    ~Context()
    {
        if (this->shell != nullptr)
        {
            delete this->shell;
            this->shell = nullptr;
        }
        if (this->screenCaptureContext != nullptr)
        {
            delete this->screenCaptureContext;
            this->screenCaptureContext = nullptr;
        }
    }
};
```

The `Shell` pointer keeps the interactive shell process alive between
`WriteShell` and `ReadShell` calls. The `ScreenCaptureContext` holds the
previous frame buffer for dirty-rectangle comparison (section 7).

Context is allocated once per connection. The main loop declares it as a local
variable before the outer `while (1)`, so it survives across reconnections in
the current code. The destructor runs `delete` on both pointers, with
null-checks to avoid double-free. Setting the pointer to `nullptr` after
deletion is defensive -- if the destructor somehow ran twice, the second pass
would be a no-op.

---

## 6. DecodeWirePath

File paths arrive over the wire as UTF-16LE (little-endian `CHAR16` sequences).
This is the universal wire encoding regardless of platform. The
`DecodeWirePath` function in `commandsHandler.cc` (lines 49-73) converts them
to the native wide-character format:

```cpp
static USIZE DecodeWirePath(PCHAR command, USIZE commandLength,
                            WCHAR *widePath, USIZE widePathSize)
{
    PCCHAR16 wirePath = (PCCHAR16)(command);
    USIZE maxChar16 = commandLength / sizeof(CHAR16);
    USIZE wireLen = 0;
    while (wireLen < maxChar16 && wirePath[wireLen] != 0)
        wireLen++;

    USIZE len = StringUtils::Char16ToWide(
        Span<const CHAR16>(wirePath, wireLen),
        Span<WCHAR>(widePath, widePathSize));

    for (USIZE i = 0; i < len; ++i)
    {
        if (widePath[i] == L'\\' || widePath[i] == L'/')
            widePath[i] = (WCHAR)PATH_SEPARATOR;
    }
    return len;
}
```

On Windows, `WCHAR` is 2 bytes, same as `CHAR16`, so `Char16ToWide` is
essentially a copy. On Linux and macOS, `WCHAR` is 4 bytes, so each `CHAR16`
gets zero-extended. The function also normalizes path separators: both `\` and
`/` become whatever `PATH_SEPARATOR` is on the current platform. POSIX
ultimately needs UTF-8 for syscalls, so there is a further conversion
downstream, but at the handler level everything stays as wide characters.

---

## 7. The Screenshot Pipeline

The screenshot command is by far the most complex handler in the agent. It
implements a full differential compression pipeline to minimize bandwidth.

### 7.1 Data Structures

From `src/beacon/screen_capture.h`, the `Graphics` struct holds all the buffers
for a single display:

```cpp
struct Graphics
{
    PRGB currentScreenshot; // current frame (raw pixels)
    PRGB screenshot;        // previous frame (for comparison)
    PUCHAR bidiff;          // binary difference map (1 byte per pixel)
    PRGB rectBuffer;        // reusable buffer for rectangle extraction
    JpegBuffer jpegBuffer;  // reusable JPEG encoding buffer
};
```

The dual-buffer design (`currentScreenshot` and `screenshot`) is essential. The
agent captures into `currentScreenshot`, diffs against `screenshot` (the
previous frame), and after sending, swaps the pointers so the current frame
becomes the previous frame for next time.

`JpegBuffer` is a reusable buffer that avoids repeated allocation:

```cpp
struct JpegBuffer
{
    PUINT8 outputBuffer = nullptr;
    UINT32 size = 0;
    UINT32 offset = 0;

    VOID Reset() { offset = 0; }
};
```

Calling `Reset()` just rewinds the offset without freeing memory. The buffer
grows as needed and persists across frames.

`GraphicsList` manages an array of `Graphics` objects, one per display.
`ScreenCaptureContext` ties everything together:

```cpp
struct ScreenCaptureContext
{
    ScreenDeviceList DeviceList;
    GraphicsList GraphicsList;
    UINT32 CurrentIndex;
    UINT32 Quality;    // JPEG quality, default 75
    UINT32 Count;
};
```

### 7.2 The Pipeline

Each screenshot command executes these stages:

**Stage 1 -- Initialize (first call only).** Enumerate displays via
`Screen::GetDevices`. Allocate two RGB buffers per display (width * height *
sizeof(RGB)), plus a diff buffer and a rect extraction buffer. This happens
once and the buffers persist in the `ScreenCaptureContext`.

**Stage 2 -- Capture.** `Screen::Capture(device, rgbBuffer)` fills the current
frame buffer with raw pixels. The platform layer handles the actual screen
capture (X11, Wayland, GDI, etc.).

**Stage 3 -- Compute binary difference.** Compare each pixel of the current
frame against the previous frame. But not with exact equality. The comparison
uses a threshold of 24:

```
For each pixel (r1,g1,b1) in current vs (r2,g2,b2) in previous:
    if |r1-r2| < 24 AND |g1-g2| < 24 AND |b1-b2| < 24:
        bidiff[i] = 0   (unchanged)
    else:
        bidiff[i] = 1   (changed)
```

Why 24? Because JPEG is lossy. The operator's display decodes each JPEG tile
and shows the result, but the agent holds the *pre-compression* raw pixels as
its "previous frame." Across frames, pixels that have not actually changed
will still differ slightly due to JPEG compression artifacts from the previous
encode cycle. A threshold of 24 filters that noise. The value is empirical --
high enough to absorb compression error, low enough to catch real changes.

**Stage 4 -- Find dirty rectangles.** Divide the screen into 64x64 pixel tiles.
Any tile containing at least one changed pixel (bidiff = 1) is marked dirty.
Adjacent dirty tiles are merged into rectangles.

**Stage 5 -- Encode and serialize.** For each dirty rectangle, extract the
region from the current frame into `rectBuffer`, JPEG-encode it, and append
to the response:

```cpp
struct Rectangle
{
    UINT32 x;
    UINT32 y;
    UINT32 sizeOfData; // size of JPEG data in bytes
    UINT8 *data;       // pointer to JPEG data
};
```

The `toBuffer` method serializes each rectangle as `{x, y, jpeg_size, jpeg_data}`
with no padding between fields.

**Stage 6 -- Swap buffers.** The current frame becomes the previous frame for
the next call. The first screenshot always transmits the entire screen because
every pixel counts as "dirty" when there is no previous frame.

The bandwidth savings are substantial. A static desktop transmits zero bytes
after the initial frame. Even with active windows, only the changed tiles get
re-encoded and sent.

---

## 8. Shell Flow

The `Shell` class in `src/beacon/shell.h` wraps a platform-specific shell
process:

```cpp
class Shell
{
private:
    ShellProcess shellProcess;
    Shell(ShellProcess &&sp) noexcept;

public:
    static Result<Shell, Error> Create() noexcept;
    Result<USIZE, Error> Write(const char *data, USIZE length) noexcept;
    Result<USIZE, Error> Read(char *buffer, USIZE capacity) noexcept;
    Result<USIZE, Error> ReadError(char *buffer, USIZE capacity) noexcept;
    ~Shell() noexcept;
};
```

On POSIX, `ShellProcess` spawns `/bin/sh` over a PTY. On Windows, it launches
`cmd.exe` with three pipes (stdin, stdout, stderr). The `Shell` class itself
is platform-agnostic -- it delegates everything to `ShellProcess`.

The operator interacts through two commands:

- **WriteShell:** sends input text to the shell's stdin.
- **ReadShell:** pulls output from stdout/stderr.

The read side uses a polling strategy: first poll with a 5-second timeout to
wait for output to begin, then subsequent polls at 100ms to catch rapid bursts
of output. Polling stops when the timeout expires or the buffer fills.

The shell process stays alive between calls. That is why it lives in the
`Context` struct (section 5) rather than being created and destroyed per
command. The operator can run `cd /tmp`, and a subsequent `ls` will list
`/tmp` because the working directory persists in the shell process.

The full round-trip: operator types in the web UI, the relay forwards the
input as a `WriteShell` command, the agent writes it to the shell process's
stdin, the shell executes it, output flows back through `ReadShell` to the
relay and then to the UI. Each leg is a separate WebSocket message.

---

## 9. Error Handling Pattern

A consistent pattern runs through every handler. The `WriteErrorResponse`
helper (line 76-79 of `commandsHandler.cc`) handles failure:

```cpp
static VOID WriteErrorResponse(PPCHAR response, PUSIZE responseLength, StatusCode code)
{
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = code;
}
```

The first four bytes of every response are a `StatusCode`:

```cpp
enum StatusCode : UINT32
{
    StatusSuccess = 0,
    StatusError = 1,
    StatusUnknownCommand = 2
};
```

The relay checks this code before interpreting the rest of the payload. If the
status is non-zero, the payload is either empty or contains an error
description. This keeps error handling uniform across all command types.

---

## 10. Putting It All Together

The architecture is deliberately simple. One loop, one dispatch table, one
ownership model. There is no event system, no message queue, no thread pool.
The agent processes one command at a time, synchronously. It finishes one
command before reading the next.

This simplicity is not accidental. Complex architectures create complex failure
modes. In position-independent code running without a standard library, every
abstraction you add is an abstraction you maintain from scratch. The agent
keeps its attack surface -- both from a security standpoint and a maintenance
standpoint -- as small as possible.

The cost of simplicity is concurrency. The agent cannot handle two commands
simultaneously. For its use case (remote administration over a WebSocket
connection), that is an acceptable tradeoff. The relay serializes commands
anyway.
