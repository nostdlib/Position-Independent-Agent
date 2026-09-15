#pragma once
#include "runtime.h"

struct JpegBuffer
{
    PUINT8 outputBuffer = nullptr;
    UINT32 size = 0;
    UINT32 offset = 0;
    // Set when an allocation failed mid-encode. JpegCallback cannot report an
    // error (JpegEncoder::Encode takes a void callback), so the flag carries
    // the failure to the caller, which must check it after Encode() and
    // discard the truncated output.
    BOOL allocationFailed = false;

    /// @brief Reset offset for reuse without freeing the underlying buffer
    /// @return void
    VOID Reset()
    {
        offset = 0;
        allocationFailed = false;
    }

    /// @brief Check if the buffer is initialized and ready for use
    /// @return true if initialized, false otherwise
    BOOL isInitialized() const{
        return outputBuffer != nullptr && size > 0;
    }

    /// @brief Initialize the buffer with a specified size if it is not already initialized
    /// @param initSize New size for the buffer if initialization is needed
    /// @return void
    VOID Initialize(UINT32 initSize){
        if(!isInitialized()){
            PUINT8 buffer = new UINT8[initSize];
            if (buffer == nullptr)
            {
                allocationFailed = true;
                return;
            }
            outputBuffer = buffer;
            size = initSize;
            offset = 0;
        }
    }

    ~JpegBuffer()
    {
        if (outputBuffer)
        {
            delete[] outputBuffer;
            outputBuffer = nullptr;
        }
    }
};

struct Graphics
{
    PRGB currentScreenshot; 
    PRGB screenshot;     
    PUCHAR bidiff;          
    PRGB rectBuffer;        
    JpegBuffer jpegBuffer;

    Graphics() : currentScreenshot(nullptr), screenshot(nullptr), bidiff(nullptr), rectBuffer(nullptr) {}

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
        if (rectBuffer)
        {
            delete[] rectBuffer;
            rectBuffer = nullptr;
        }
    }

    BOOL IsInitialized() const
    {
        return currentScreenshot != nullptr && screenshot != nullptr && bidiff != nullptr && rectBuffer != nullptr;
    }

    VOID Init(const ScreenDevice &device)
    {
        USIZE pixelCount = (USIZE)device.Width * device.Height;
        if (currentScreenshot == nullptr)
        {
            currentScreenshot = new RGB[pixelCount];
        }
        if (screenshot == nullptr)
        {
            screenshot = new RGB[pixelCount];
        }
        if (bidiff == nullptr)
        {
            bidiff = new UINT8[pixelCount];
        }
        if (rectBuffer == nullptr)
        {
            rectBuffer = new RGB[pixelCount];
        }
    }
};

struct GraphicsList
{
    Graphics *graphicsArray; 
    UINT32 count;

    GraphicsList() : graphicsArray(nullptr), count(0) {}

    ~GraphicsList()
    {
        if (graphicsArray)
        {
            delete[] graphicsArray;
            graphicsArray = nullptr;
        }
        
        count = 0;
    }
    
    BOOL IsInitialized() const
    {
        return graphicsArray != nullptr && count > 0;
    }

    VOID Init(UINT32 Count)
    {
        if(graphicsArray != nullptr){
            if(count == Count)
                return;
                
            delete[] graphicsArray;
            graphicsArray = nullptr;
            count = 0;
        }

        graphicsArray = new Graphics[Count];
        count = Count;
    }
};

struct ScreenCaptureContext
{
    ScreenDeviceList DeviceList;
    GraphicsList GraphicsList;
    UINT32 CurrentIndex;
    UINT32 Quality;
    UINT32 Count;

    ScreenCaptureContext() : CurrentIndex(0), Quality(75), Count(0)
    {
        DeviceList.Devices = nullptr;
        DeviceList.Count = 0;
        GraphicsList.graphicsArray = nullptr;
        GraphicsList.count = 0;
    }

    ~ScreenCaptureContext()
    {
        DeviceList.Free();
    }
};


struct Rectangle
{
    UINT32 x;
    UINT32 y;
    UINT32 sizeOfData; // Size of the jpeg data in bytes
    UINT8 *data;

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