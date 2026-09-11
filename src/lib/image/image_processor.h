/**
 * @file image_processor.h
 * @brief Image Processing Utilities
 *
 * @details Provides tile-based dirty region detection, binary image
 * differencing, and related utilities for position-independent execution.
 *
 * @ingroup runtime
 *
 * @defgroup image Image Processing
 * @ingroup runtime
 * @{
 */

#pragma once

#include "platform/platform.h"
#include "core/types/rgb.h"

/// @brief Axis-aligned rectangle representing a dirty screen region
struct DirtyRect
{
	UINT32 X;
	UINT32 Y;
	UINT32 Width;
	UINT32 Height;
};

struct DirtyRectResult
{
	DirtyRect *Rects;
	UINT32 Count;

	VOID Free()
	{
		if (Rects)
		{
			delete[] Rects;
			Rects = nullptr;
		}
		Count = 0;
	}
};

/**
 * @class ImageProcessor
 * @brief Image processing utilities: differencing and dirty region detection
 *
 * @details All methods are static and stateless. The class is stack-only
 * (no heap allocation of the class itself).
 */
class ImageProcessor
{
public:
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;

	/**
	 * @brief Calculate per-pixel binary difference between two RGB images
	 *
	 * @param image1 First image (width * height RGB pixels)
	 * @param image2 Second image (same dimensions)
	 * @param width Image width in pixels
	 * @param height Image height in pixels
	 * @param biDiff Output: 1 where pixels differ beyond threshold, 0 otherwise
	 * @param threshold Sum-of-absolute-differences threshold per pixel (0 = exact match)
	 */
	static VOID CalculateBiDifference(
		Span<const RGB> image1,
		Span<const RGB> image2,
		UINT32 width,
		UINT32 height,
		Span<UINT8> biDiff,
		UINT32 threshold = 0);

	/**
	 * @brief Find dirty rectangles using tile-based detection with row merging
	 *
	 * @details Divides the binary difference image into tiles and marks each
	 * tile as dirty if any pixel within it is nonzero. Adjacent dirty tiles
	 * in the same row are merged into horizontal runs, then vertically
	 * adjacent runs with matching X span are merged into larger rectangles.
	 *
	 * @param biDiff Binary difference image (0 = unchanged, nonzero = changed)
	 * @param width Image width in pixels
	 * @param height Image height in pixels
	 * @param tileSize Tile size in pixels (must be > 0, typically 32 or 64)
	 * @return Ok(DirtyRectResult) on success, or error on allocation failure
	 */
	[[nodiscard]] static Result<DirtyRectResult, Error> FindDirtyRects(
		Span<const UINT8> biDiff,
		UINT32 width,
		UINT32 height,
		UINT32 tileSize);
};

/** @} */ // end of image group
