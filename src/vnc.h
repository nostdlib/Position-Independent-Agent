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