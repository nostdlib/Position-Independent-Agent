#pragma once
#include "runtime.h"

struct Graphics
{
    PRGB currentScreenshot; // Pointer to the current screenshot
    PRGB screenshot;        // Pointer to the screenshot of the display
    PUCHAR bidiff;          // Pointer to the binary difference data

    ~Graphics()
    {
        if (currentScreenshot)
        {
            delete[] currentScreenshot;
            currentScreenshot = nullptr;
        }
        if (screenshot)
        {
            delete[] screenshot;
            screenshot = nullptr;
        }
        if (bidiff)
        {
            delete[] bidiff;
            bidiff = nullptr;
        }
    }
};

struct GraphicsList
{
    Graphics *graphicsArray; // Array of Graphics structures
    UINT32 count;            // Number of Graphics structures in the array

    ~GraphicsList()
    {
        if (graphicsArray)
        {
            delete[] graphicsArray;
            graphicsArray = nullptr;
        }
    }
};

struct VNCContext
{
    ScreenDeviceList DeviceList;
    GraphicsList GraphicsList;
    UINT32 CurrentIndex;
    UINT32 Quality;
    UINT32 Count;

    VNCContext() : CurrentIndex(0), Quality(75), Count(0)
    {
        DeviceList.Devices = nullptr;
        DeviceList.Count = 0;
        GraphicsList.graphicsArray = nullptr;
        GraphicsList.count = 0;
    }

    ~VNCContext()
    {
        DeviceList.Free();
        // GraphicsList will be automatically freed by its destructor
    }
};

// Structure for representing a rectangle, which consists of x and y coordinates and an array of RGB data.
struct Rectangle
{
    UINT32 x;
    UINT32 y;
    UINT32 sizeOfData; // Size of the jpeg data in bytes
    UINT8 *data;       // Pointer to hold the JPEG data.

    Rectangle(UINT32 x, UINT32 y, UINT32 sizeOfData, UINT8 *data)
        : x(x), y(y), sizeOfData(sizeOfData), data(data) {}

    USIZE toBuffer(PUINT8 buffer) const
    {
        USIZE bytesWritten = 0;
        Memory::Copy(buffer, &x, sizeof(x));
        bytesWritten += sizeof(x);
        Memory::Copy(buffer + bytesWritten, &y, sizeof(y));
        bytesWritten += sizeof(y);
        Memory::Copy(buffer + bytesWritten, &sizeOfData, sizeof(sizeOfData));
        bytesWritten += sizeof(sizeOfData);
        Memory::Copy(buffer + bytesWritten, data, sizeOfData);
        bytesWritten += sizeOfData;
        return bytesWritten;
    }
};

struct JpegBuffer
{
    PUINT8 outputBuffer = nullptr;
    UINT32 size = 0;
    UINT32 offset = 0;

    ~JpegBuffer()
    {
        if (outputBuffer)
        {
            delete[] outputBuffer;
            outputBuffer = nullptr;
        }
    }
};