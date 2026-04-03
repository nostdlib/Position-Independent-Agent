#pragma once
#include "runtime.h"

struct JpegBuffer
{
    PUINT8 outputBuffer = nullptr;
    UINT32 size = 0;
    UINT32 offset = 0;

    /// @brief Reset offset for reuse without freeing the underlying buffer
    /// @return void
    VOID Reset()
    {
        offset = 0;
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
            outputBuffer = new UINT8[initSize];
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
    PRGB currentScreenshot; // Pointer to the current screenshot
    PRGB screenshot;        // Pointer to the screenshot of the display
    PUCHAR bidiff;          // Pointer to the binary difference data
    PRGB rectBuffer;        // Reusable buffer for rectangle extraction
    JpegBuffer jpegBuffer;  // Reusable JPEG encoding buffer (persists across frames)

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

    /// @brief Check if the Graphics instance is initialized by verifying that all necessary buffers are allocated
    /// @return true if initialized, false otherwise
    BOOL IsInitialized() const
    {
        return currentScreenshot != nullptr && screenshot != nullptr && bidiff != nullptr && rectBuffer != nullptr;
    }

    /// @brief Initialize the Graphics instance by allocating necessary buffers based on the provided screen device's dimensions
    /// @param device Screen device containing the dimensions for buffer allocation
    /// @return void
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
    Graphics *graphicsArray; // Array of Graphics structures
    UINT32 count;            // Number of Graphics structures in the array

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
    
    /// @brief Check if the GraphicsList is initialized by verifying that the graphics array is allocated and has a positive count
    /// @return true if initialized, false otherwise
    BOOL IsInitialized() const
    {
        return graphicsArray != nullptr && count > 0;
    }

    /// @brief Initialize the GraphicsList with a specified count of Graphics structures
    /// @param Count Number of Graphics structures to allocate
    /// @return void
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