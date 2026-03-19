# pyloader

Cross-platform shellcode loader for position-independent code. Downloads pre-built agents from GitHub Releases (or loads from a local file) and executes them.

Requires **Python 2.7+** or **3.2+** with no third-party dependencies.

## Usage

```bash
# Auto-detect platform and arch, download latest preview build:
python loader.py

# Download a specific release:
python loader.py --tag v1.0.0

# Override architecture (e.g. run i386 build on x86_64 host):
python loader.py --arch i386

# Load from a local .bin file (--arch required):
python loader.py --arch x86_64 output.bin
```

## How it works

| Platform | Method | Details |
|----------|--------|---------|
| Linux, macOS, FreeBSD, Solaris, Android, iOS | `mmap` + `mprotect` | Maps RW, writes shellcode, flips to RX, calls entry point in-process |
| Windows | Process injection | Creates suspended `cmd.exe`, allocates RWX in remote process, injects via `CreateRemoteThread` |

On POSIX systems the shellcode must match the Python interpreter's bitness (a 32-bit Python downloads 32-bit shellcode). On Windows the injection targets a native-arch process, so any Python bitness works.

## Supported platforms

Auto-detection covers every platform/architecture the agent builds for:

| OS | Architectures |
|----|---------------|
| Windows | i386, x86_64, armv7a, aarch64 |
| macOS | x86_64, aarch64 |
| Linux | i386, x86_64, armv7a, aarch64, riscv32, riscv64, mips64 |
| Solaris | i386, x86_64, aarch64 |
| FreeBSD | i386, x86_64, aarch64, riscv64 |
| Android | x86_64, armv7a, aarch64 |
| iOS | aarch64 |

## SSL

SSL certificate verification is disabled. The loader downloads unsigned shellcode from public GitHub Releases — verification adds no security value and breaks on hosts with outdated CA stores (e.g. Windows 7, Solaris).
