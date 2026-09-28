#pragma once
#include "runtime.h"
#include "logger.h"

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

    /// @brief Grow the backing buffer to at least the requested size, keeping contents
    /// @param needed Minimum capacity in bytes
    /// @return void (sets allocationFailed on failure)
    VOID EnsureCapacity(UINT32 needed)
    {
        if (size >= needed || allocationFailed)
            return;
        PUINT8 grown = new UINT8[needed];
        if (grown == nullptr)
        {
            allocationFailed = true;
            return;
        }
        if (outputBuffer != nullptr)
        {
            Memory::Copy(grown, outputBuffer, offset);
            delete[] outputBuffer;
        }
        outputBuffer = grown;
        size = needed;
    }

    /// @brief Pre-size for a whole-frame or single-rect JPEG encode
    /// @param width Region width in pixels
    /// @param height Region height in pixels
    /// @return void (sets allocationFailed on failure)
    /// @note Screen-content JPEG at q75 fits well under 1/8 of raw RGB plus
    ///       header slack; underestimates still grow via EnsureCapacity
    VOID ReserveForImage(UINT32 width, UINT32 height)
    {
        EnsureCapacity((UINT32)((USIZE)width * height * 3 / 8 + 4096));
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

// Motion-budget pacing: under heavy motion the whole screen goes dirty and a
// full-area reply costs near-full-frame encode time, collapsing frame pacing.
// The budget bounds the encoded area per reply; overflow rects are deferred.
// Integer arithmetic throughout — floating constants pool into .rdata on
// PE/COFF i386 and break position-independence.
static constexpr UINT64 MotionBudgetTargetUs = 25000;  // target per-reply handler time in µs
static constexpr UINT64 MotionBudgetGrowUs = 15000;    // grow below 60% of the target
static constexpr UINT64 MotionBudgetEncodeShareUs = 20000; // post-capture (diff+encode) allowance in µs
static constexpr UINT64 MotionBudgetStartNum = 2;      // seed budget = framePixels * 2/5
static constexpr UINT64 MotionBudgetStartDen = 5;
static constexpr UINT64 MotionBudgetMinNum = 1;        // budget floor = framePixels / 10
static constexpr UINT64 MotionBudgetMinDen = 10;

struct Graphics
{
    PRGB currentScreenshot;
    PRGB screenshot;
    JpegBuffer jpegBuffer;
    // Persistent incremental-reply packet: Reset() keeps the capacity across
    // frames; Release() (the per-reply ownership handoff) empties it, so the
    // handler re-Init()s on the next request
    Buffer<CHAR> packet;
    PVOID captureState;  // Opaque per-display resources from Screen::CreateCaptureState
    INT64 encodeEmaUs;   // EMA of recent per-reply encode times in µs; 0 = no sample yet
    USIZE areaBudgetPixels; // Encoded-area budget in pixels; 0 = seed on first use

    Graphics() : currentScreenshot(nullptr), screenshot(nullptr), captureState(nullptr), encodeEmaUs(0), areaBudgetPixels(0) {}

    /// @brief Effective encoded-area budget for this frame, seeded and clamped
    /// @param framePixels Total pixels in the frame
    /// @return Budget in pixels, between the floor and the full frame
    USIZE AreaBudget(USIZE framePixels)
    {
        if (areaBudgetPixels == 0)
            areaBudgetPixels = framePixels * MotionBudgetStartNum / MotionBudgetStartDen;
        USIZE floorPixels = framePixels / MotionBudgetMinDen;
        if (areaBudgetPixels > framePixels)
            areaBudgetPixels = framePixels;
        if (areaBudgetPixels < floorPixels)
            areaBudgetPixels = floorPixels;
        return areaBudgetPixels;
    }

    /// @brief Fold one reply's handler time into the EMA and adapt the budget
    /// @param handlerNs Wall time of the whole capture+diff+encode handler
    /// @param captureNs The capture stage's share (irreducible: GDI cost)
    /// @param framePixels Total pixels in the frame (growth clamp)
    VOID AdaptMotionBudget(UINT64 handlerNs, [[maybe_unused]] UINT64 captureNs, USIZE framePixels)
    {
        INT64 totalUs = (INT64)(handlerNs / 1000);
        // EMA with weight 1/4 (shift-free integer form; truncation is fine here)
        encodeEmaUs = (encodeEmaUs <= 0) ? totalUs : encodeEmaUs + (totalUs - encodeEmaUs) / 4;

        // Deferral is DISABLED: encoding a frame's rects from staggered points
        // in time tears video content (regions of the screen visibly out of
        // order), which is worse than the pacing it saved. The SIMD encoder
        // removed the overload the budget was built to absorb. The budget
        // stays pinned at the full frame; the EMA and the [shot] log line
        // remain for diagnostics.
        USIZE next = framePixels;

        USIZE floorPixels = framePixels;
        if (next > framePixels)
            next = framePixels;
        if (next < floorPixels)
            next = floorPixels;
        if (next != areaBudgetPixels)
        {
            LOG_DEBUG("Motion budget %u -> %u px (total EMA %d us)", (UINT32)areaBudgetPixels, (UINT32)next, (INT32)encodeEmaUs);
            areaBudgetPixels = next;
        }
    }

    // Drop the persistent capture state; the next capture re-creates it
    VOID ReleaseCaptureState()
    {
        if (captureState != nullptr)
        {
            Screen::DestroyCaptureState(captureState);
            captureState = nullptr;
        }
    }

    // Make the current frame the comparison base for the next request
    // (pointer swap — no full-frame copy)
    VOID SwapFrames()
    {
        PRGB previous = currentScreenshot;
        currentScreenshot = screenshot;
        screenshot = previous;
    }

    ~Graphics()
    {
        ReleaseCaptureState();
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
    }

    BOOL IsInitialized() const
    {
        return currentScreenshot != nullptr && screenshot != nullptr;
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