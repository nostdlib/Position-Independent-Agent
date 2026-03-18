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

// =============================================================================
// Wire helpers
// =============================================================================

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

    for (USIZE i = 0; i < len; ++i)
    {
        if (widePath[i] == L'\\' || widePath[i] == L'/')
            widePath[i] = (WCHAR)PATH_SEPARATOR;
    }
    return len;
}

// Writes a simple error response with the given status code
static VOID WriteErrorResponse(PPCHAR response, PUSIZE responseLength, StatusCode code)
{
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = code;
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

VOID Handle_GetSystemInfoCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    SystemInfo info;
    GetSystemInfo(&info);

    AgentBuildInfo buildInfo;
    buildInfo.BuildNumber = AGENT_BUILD_NUMBER;
    const CHAR commitHash[] = AGENT_COMMIT_HASH;
    Memory::Copy(buildInfo.CommitHash, commitHash, sizeof(commitHash));

    *responseLength = sizeof(UINT32) + sizeof(SystemInfo) + sizeof(AgentBuildInfo);
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &info, sizeof(SystemInfo));
    Memory::Copy(*response + sizeof(UINT32) + sizeof(SystemInfo), &buildInfo, sizeof(AgentBuildInfo));

    LOG_INFO("GetSystemInfo: hostname=%s, arch=%s, platform=%s, build=%u, commit=%s",
             info.Hostname, info.Architecture, info.Platform, buildInfo.BuildNumber, buildInfo.CommitHash);
}

VOID Handle_GetDirectoryContentCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetDirectoryContentCommand.");
    // Buffer to hold the path from command
    WCHAR directoryPath[1024];
    // Decoding path from command
    DecodeWirePath(command, commandLength, directoryPath, 1024);
    LOG_INFO("GetDirectoryContent: %ws", directoryPath);

    // Create a DirectoryIterator for the specified path and validate it
    auto result = DirectoryIterator::Create(directoryPath);
    if (!result.IsOk())
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    // Iterator successfully created, so we can now read entries
    DirectoryIterator &iter = result.Value();
    Vector<DirectoryEntry> entries;
    if (!entries.Init())
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    // Iterate through the directory entries, skipping "." and "..", and add them to the vector
    while (iter.Next())
    {
        const DirectoryEntry &entry = iter.Get();
        if (IsDotEntry(entry))
            continue;
        if (!entries.Add(entry))
        {
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        LOG_INFO("Directory entry added: %ws", entry.Name);
    }

    // Prepare the response buffer - writing entry count, status code and array of WireDirectoryEntry structures
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
    LOG_INFO("Directory content retrieved successfully with %llu entries", entryCount);
}

// Reads a chunk of file content
VOID Handle_GetFileContentCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetFileContentCommand.");
    // Getting parameters from command buffer: read count, offset and file path
    UINT64 readCount = *(PUINT64)(command);
    UINT64 offset = *(PUINT64)(command + sizeof(UINT64));
    LOG_INFO("Reading file content with offset: %llu and count: %llu.", offset, readCount);

    // Decoding file path from command buffer
    USIZE pathOffset = sizeof(UINT64) + sizeof(UINT64);
    WCHAR filePath[1024];
    DecodeWirePath(command + pathOffset, commandLength > pathOffset ? commandLength - pathOffset : 0, filePath, 1024);
    LOG_INFO("GetFileContent: %ws offset=%llu count=%llu", filePath, offset, readCount);

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    LOG_INFO("File opened successfully: %ws", filePath);

    // Prepare the response buffer - writing status code, bytes read and file content chunk
    File &file = openResult.Value();
    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)readCount;
    *response = new CHAR[*responseLength];

    (void)file.SetOffset((USIZE)offset);
    auto readResult = file.Read(Span<UINT8>((UINT8 *)(*response + sizeof(UINT32) + sizeof(UINT64)), (USIZE)readCount));
    UINT32 bytesRead = readResult ? readResult.Value() : 0;

    *(PUINT32)*response = StatusCode::StatusSuccess;
    *(PUINT64)(*response + sizeof(UINT32)) = bytesRead;

    LOG_INFO("File content read successfully for %llu bytes requested, %llu bytes read", readCount, bytesRead);
}

// Computes the SHA-256 hash of a file chunk
VOID Handle_GetFileChunkHashCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetFileChunkHashCommand.");
    // Getting parameters from command buffer: chunk size, offset and file path
    UINT64 chunkSize = *(PUINT64)(command);
    UINT64 offset = *(PUINT64)(command + sizeof(UINT64));

    LOG_INFO("Computing file chunk hash with offset: %llu and chunk size: %llu.", offset, chunkSize);

    // Decoding file path from command buffer
    USIZE hashPathOffset = sizeof(UINT64) + sizeof(UINT64);
    WCHAR filePath[1024];
    DecodeWirePath(command + hashPathOffset, commandLength > hashPathOffset ? commandLength - hashPathOffset : 0, filePath, 1024);
    LOG_INFO("GetFileChunkHash: %ws chunkSize=%llu offset=%llu", filePath, chunkSize, offset);

    // Attempt to open the file and validate the result
    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    LOG_INFO("File opened successfully: %ws", filePath);

    File &file = openResult.Value();
    // Allocating a buffer for reading file chunks.
    UINT64 bufferSize = Math::Min((UINT64)chunkSize, (UINT64)0xffff);
    PUINT8 buffer = new UINT8[bufferSize];

    SHA256 sha256;
    USIZE totalRead = 0;
    // Read file in small chanks and update the hash untill we read the requested count or reach the end of file
    while (totalRead < chunkSize)
    {
        UINT64 bytesToRead = Math::Min(bufferSize, chunkSize - totalRead);
        LOG_INFO("Reading file chunk with offset: %llu and count: %llu.", offset + totalRead, bytesToRead);
        (void)file.SetOffset((USIZE)(offset + totalRead));
        auto readResult = file.Read(Span<UINT8>(buffer, (USIZE)bytesToRead));
        UINT32 bytesRead = readResult ? readResult.Value() : 0;
        if (bytesRead == 0)
            break;
        sha256.Update(Span<const UINT8>(buffer, bytesRead));
        totalRead += bytesRead;
    }
    delete[] buffer;
    // Prepare the response buffer - writing status code and SHA-256 digest of the file chunk
    *responseLength += SHA256_DIGEST_SIZE;
    *response = new CHAR[*responseLength];

    UINT8 digest[SHA256_DIGEST_SIZE];
    sha256.Final(Span<UINT8, SHA256_DIGEST_SIZE>(digest));

    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), digest, SHA256_DIGEST_SIZE);
    LOG_INFO("GetFileChunkHash: hashed %llu bytes", (UINT64)totalRead);
}

// Writes a command to the shell
VOID Handle_WriteShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();
        if (!shellResult)
        {
            LOG_ERROR("Failed to create shell instance");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
    }

    while (commandLength > 0 && command[commandLength - 1] == '\0')
        commandLength--;

    auto writeResult = context->shell->Write(command, commandLength);
    if (!writeResult)
    {
        LOG_ERROR("Failed to write command to shell");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    LOG_INFO("Command written to shell successfully, bytes written: %llu", writeResult.Value());

    // Prepare the response buffer - writing status code
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
}

// Reads a chunk of data from the shell's stdout
VOID Handle_ReadShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();
        if (!shellResult)
        {
            LOG_ERROR("Failed to create shell instance");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
        LOG_INFO("Shell instance created successfully");
    }

    // Buffer to hold the data read from the shell
    CHAR buffer[4096];
    auto readResult = context->shell->Read(buffer, sizeof(buffer));
    if (!readResult)
    {
        LOG_ERROR("Failed to read from shell");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    USIZE bytesRead = readResult.Value() + 1;
    *responseLength += bytesRead;
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    StringUtils::Copy(Span<CHAR>(*response + sizeof(UINT32), bytesRead), Span<const CHAR>(buffer, bytesRead));
}

// Gets the list of display devices and their information
VOID Handle_GetDisplaysCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    if (context->vncContext == nullptr)
        context->vncContext = new VNCContext();

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
    context->vncContext->DeviceList = deviceList;
    // Prepare the response buffer - writing status code, device count and array of ScreenDevice structures
    *responseLength += sizeof(deviceList.Count) + (USIZE)(deviceList.Count * sizeof(ScreenDevice));
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &deviceList.Count, sizeof(deviceList.Count));
    Memory::Copy(*response + sizeof(UINT32) + sizeof(deviceList.Count), deviceList.Devices, (USIZE)(deviceList.Count * sizeof(ScreenDevice)));

    LOG_INFO("GetDisplays: %u display(s)", deviceList.Count);
}

// Callback function for JPEG encoding - called by the encoder to write encoded data chunks
VOID JpegCallback(PVOID context, PVOID data, INT32 size)
{
    JpegBuffer *jpegBuffer = (JpegBuffer *)context;

    if (data == nullptr)
    {
        if (jpegBuffer->outputBuffer == nullptr)
        {
            jpegBuffer->size = (UINT32)size;
            jpegBuffer->outputBuffer = new UINT8[jpegBuffer->size];
        }
    }

    if (jpegBuffer->offset + size > jpegBuffer->size)
    {
        UINT32 newSize = Math::Max(jpegBuffer->size * 2, jpegBuffer->size + size);
        PUINT8 newBuffer = new UINT8[newSize];
        Memory::Copy(newBuffer, jpegBuffer->outputBuffer, jpegBuffer->offset);
        delete[] jpegBuffer->outputBuffer;
        jpegBuffer->outputBuffer = newBuffer;
        jpegBuffer->size = newSize;
    }
    // Copy the encoded data chunk into the buffer and update the offset
    Memory::Copy(jpegBuffer->outputBuffer + jpegBuffer->offset, data, (USIZE)size);
    jpegBuffer->offset += (UINT32)size;
}

// Gets a screenshot of the specified display device
VOID Handle_GetScreenshotCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    auto displayIndex = *(PUINT32)(command);
    auto quality = *(PUINT32)(command + sizeof(UINT32));
    auto isFullScreen = *(PUINT32)(command + sizeof(UINT32) + sizeof(UINT32));
    LOG_INFO("Handling GetScreenshotCommand for display index: %u, quality: %u, isFullScreen: %u", displayIndex, quality, isFullScreen);

    // Ensure the VNC context exists - create it if it doesn't, and validate the result
    if (context->vncContext == nullptr)
        context->vncContext = new VNCContext();

    // Getting the device list
    if (context->vncContext->DeviceList.Count == 0)
    {
        auto displays = Screen::GetDevices();
        if (!displays)
        {
            LOG_ERROR("Failed to enumerate display devices");
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->vncContext->DeviceList = displays.Value();
        LOG_INFO("Display devices enumerated successfully with %u display(s)", context->vncContext->DeviceList.Count);
    }

    const ScreenDevice &device = context->vncContext->DeviceList.Devices[displayIndex];

    if (context->vncContext->GraphicsList.count == 0)
        context->vncContext->GraphicsList.Init(context->vncContext->DeviceList.Count);

    Graphics &graphics = context->vncContext->GraphicsList.graphicsArray[displayIndex];

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
        if (encodeResult.IsErr())
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
        *(PUINT32)*response = StatusCode::StatusSuccess;
        Memory::Copy(*response + sizeof(UINT32), &countOfSegments, sizeof(UINT32));
        rect.toBuffer((UINT8 *)*response + sizeof(UINT32) + sizeof(UINT32));
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
    USIZE offset = sizeof(UINT32) + sizeof(UINT32);

    // Pre-allocate response buffer with generous initial capacity to avoid per-rect reallocation.
    USIZE packetCapacity = *responseLength + sizeof(UINT32) + (USIZE)device.Width * device.Height / 2;
    PCHAR packet = new CHAR[packetCapacity];

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
        if (encodeResult.IsErr())
        {
            delete[] packet;
            dirtyRects.Free();
            LOG_ERROR("Failed to encode the screenshot for display index: %u", displayIndex);
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        USIZE rectEntrySize = graphics.jpegBuffer.offset + sizeof(UINT32) * 3; // x + y + sizeOfData + jpegData
        // Grow the packet buffer if needed (double capacity until it fits)
        if (offset + rectEntrySize > packetCapacity)
        {
            USIZE newCapacity = packetCapacity;
            while (offset + rectEntrySize > newCapacity)
                newCapacity *= 2;
            auto newPacket = new CHAR[newCapacity];
            Memory::Copy(newPacket, packet, offset);
            delete[] packet;
            packet = newPacket;
            packetCapacity = newCapacity;
        }

        Rectangle rect(dr.X, dr.Y, graphics.jpegBuffer.offset, graphics.jpegBuffer.outputBuffer);
        offset += rect.toBuffer((UINT8 *)packet + offset);
    }

    // Copy the current screenshot to the screenshot buffer for the next comparison
    Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

    *(PUINT32)(packet + sizeof(UINT32)) = countOfRects;
    *response = packet;
    *responseLength = offset;
    *(PUINT32)*response = StatusCode::StatusSuccess;

    dirtyRects.Free();
}
