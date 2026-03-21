[< Back to Platform](../README.md) | [< Back to Project Root](../../../README.md)

# Console I/O

Platform-independent text output and structured logging, implemented directly over kernel syscalls without any CRT dependency.

## How Console Output Works

### The UTF-16 → UTF-8 Problem

The runtime uses wide strings (`WCHAR*`, UTF-16LE) internally for compatibility with Windows and UEFI. But POSIX terminals expect UTF-8. The `Console::Write(Span<const WCHAR>)` overload handles this with a streaming codepoint-by-codepoint conversion:

```
Write(L"Hello 世界")
  │
  ├─ Windows: WriteConsoleW() — native UTF-16, no conversion needed
  │
  ├─ POSIX:
  │    ├─ For each input character:
  │    │    CodepointToUTF8(text, index, bytes[4]) → byteCount (1-4)
  │    │    Append bytes to 256-byte stack buffer
  │    │    If buffer full → flush via write(STDOUT_FILENO) syscall
  │    └─ Flush remaining bytes
  │
  └─ UEFI: Direct OutputString() — CHAR16 is the native format
```

The conversion handles surrogate pairs (codepoints > U+FFFF) and produces valid multi-byte UTF-8 sequences. The 256-byte buffer amortizes syscall overhead — each `write()` syscall has non-trivial kernel transition cost.

### Windows Output Path

Windows uses two different output mechanisms:

- **Narrow strings** (`CHAR*`): `ZwWriteFile` to the `StandardOutput` handle obtained from `PEB→ProcessParameters→StandardOutput`. This bypasses the Win32 `WriteFile` API entirely.
- **Wide strings** (`WCHAR*`): `WriteConsoleW` resolved dynamically from kernel32.dll via PEB export resolution. Native UTF-16 — no conversion needed.

### UEFI Output Path

UEFI's `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL→OutputString` accepts `CHAR16*` (equivalent to `WCHAR*`). Narrow strings are widened character-by-character before output. The protocol pointer comes from `EFI_SYSTEM_TABLE→ConOut`.

## Formatted Output

`Console::WriteFormatted<TChar, Args...>(format, args...)` is a type-safe variadic template formatter. Unlike `printf`, format specifier dispatch is resolved at compile time — the compiler generates specialized code for each argument type combination.

## Logger

Structured logging with ANSI color escapes and timestamps:

```
\033[0;32m[INF] [14:30:45] Connection established\033[0m
         ─────  ────────  ──────────────────────
         green  DateTime  message
         prefix ::Now()
```

| Level | Color Code | Prefix |
|---|---|---|
| `Logger::Info` | `\033[0;32m` (green) | `[INF]` |
| `Logger::Warning` | `\033[0;33m` (yellow) | `[WRN]` |
| `Logger::Error` | `\033[0;31m` (red) | `[ERR]` |
| `Logger::Debug` | `\033[0;36m` (cyan) | `[DBG]` |

When `ENABLE_LOGGING` is disabled at compile time, all logging calls are eliminated — zero overhead.

## Platform Implementations

| Platform | Narrow Path | Wide Path |
|---|---|---|
| **Windows** | `ZwWriteFile` to PEB→StandardOutput | `WriteConsoleW` (native Unicode) |
| **POSIX** (all 6) | `write(STDOUT_FILENO)` syscall | UTF-16→UTF-8 conversion, then `write()` |
| **UEFI** | Widen to CHAR16, then `OutputString` | Direct `ConOut→OutputString` |
