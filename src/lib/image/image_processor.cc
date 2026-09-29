/**
 * @file image_processor.cc
 * @brief Image Processing Utilities — Implementation
 *
 * @details Implements binary image differencing and tile-based dirty region
 * detection for real-time screen streaming.
 */

#include "lib/image/image_processor.h"
#include "core/containers/vector.h"

#if defined(ARCHITECTURE_X86_64)
// SSE2 via the project's own declarations — no compiler headers
#include "core/compiler/sse2.h"
#endif

// ============================================================
//  Absolute value helper (no CRT dependency)
// ============================================================

[[nodiscard]] static constexpr INT32 AbsInt(INT32 x)
{
	return x < 0 ? -x : x;
}

/// @brief Per-pixel sum of absolute channel differences — the single dirty
/// predicate shared by the bidiff map and the fused tile scan
[[nodiscard]] static constexpr UINT32 PixelSad(const RGB &a, const RGB &b)
{
	return (UINT32)AbsInt((INT32)a.Red - (INT32)b.Red) +
	       (UINT32)AbsInt((INT32)a.Green - (INT32)b.Green) +
	       (UINT32)AbsInt((INT32)a.Blue - (INT32)b.Blue);
}

/// @brief Scan n pixels of one row segment left to right — the scalar
///        reference path shared by the fused scan's SIMD-chunk fallback and
///        row tail (and by every non-x86_64 build)
/// @param cur8 Current-frame bytes, 3 per pixel, pixel i at cur8 + 3*i
/// @param prev8 Previous-frame bytes, same layout
/// @param n Pixel count (must be >= 1; the last pixel may be the row's)
/// @param threshold Per-pixel SAD threshold
/// @param anyDrift Set when any pixel has a nonzero sub-threshold SAD
/// @return true when a pixel passes the threshold (stops at the first one)
static BOOL ScanPixelsScalar(
	const UINT8 *cur8,
	const UINT8 *prev8,
	UINT32 n,
	UINT32 threshold,
	BOOL &anyDrift)
{
	for (UINT32 i = 0; i + 1 < n; ++i, cur8 += 3, prev8 += 3)
	{
#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_I386)
		// 4-byte pre-compare via one unaligned word read per side: the high
		// byte belongs to the next pixel on both sides, so an equal word
		// implies an identical pixel and the SAD is skipped
		if (*(const UINT32 *)cur8 == *(const UINT32 *)prev8)
			continue;
#endif

		UINT32 sad = PixelSad(*(const RGB *)cur8, *(const RGB *)prev8);
		if (sad > threshold)
			return true;
		if (sad != 0)
			anyDrift = true;
	}

	// Final pixel: SAD only — its 4-byte read could cross into the next
	// row, or past the buffer on the frame's last row
	UINT32 sad = PixelSad(*(const RGB *)cur8, *(const RGB *)prev8);
	if (sad > threshold)
		return true;
	if (sad != 0)
		anyDrift = true;
	return false;
}

// ============================================================
//  Public API
// ============================================================

/// @brief Calculate per-pixel binary difference between two RGB images
/// @param image1 First image (width * height RGB pixels)
/// @param image2 Second image (same dimensions)
/// @param width Image width in pixels
/// @param height Image height in pixels
/// @param biDiff Output binary difference image (width * height)
/// @param threshold Sum-of-absolute-differences threshold per pixel.
///        Pixels whose SAD is <= threshold are considered identical.
///        Use a small value (e.g. 24–32) to ignore JPEG ringing artifacts
///        from the previous frame's encode/decode cycle.
/// @return void
VOID ImageProcessor::CalculateBiDifference(
	Span<const RGB> image1,
	Span<const RGB> image2,
	UINT32 width,
	UINT32 height,
	Span<UINT8> biDiff,
	UINT32 threshold)
{
	UINT32 totalPixels = width * height;

	if (threshold == 0)
	{
		// Fast path: exact comparison using 32-bit reads.
		// The mask 0x00FFFFFF isolates the 3 RGB bytes on little-endian.
		// Stop 1 pixel early to avoid reading past the buffer on the last pixel.
		auto p1 = (const UINT8 *)image1.Data();
		auto p2 = (const UINT8 *)image2.Data();

		UINT32 i = 0;
		if (totalPixels > 1)
		{
			for (; i < totalPixels - 1; ++i)
			{
				UINT32 v1 = *(const UINT32 *)(p1 + i * 3);
				UINT32 v2 = *(const UINT32 *)(p2 + i * 3);
				biDiff[i] = ((v1 ^ v2) & 0x00FFFFFF) ? 1 : 0;
			}
		}

		// Last pixel: per-byte comparison to avoid out-of-bounds read
		if (totalPixels > 0)
		{
			biDiff[i] = (image1[i].Red != image2[i].Red ||
						 image1[i].Green != image2[i].Green ||
						 image1[i].Blue != image2[i].Blue) ? 1 : 0;
		}
	}
	else
	{
		// Threshold path: sum of absolute differences per channel.
		// Ignores minor pixel differences caused by JPEG compression artifacts.
		for (UINT32 i = 0; i < totalPixels; ++i)
			biDiff[i] = (PixelSad(image1[i], image2[i]) > threshold) ? 1 : 0;
	}
}

// ============================================================
//  Tile-based dirty region detection
// ============================================================

/// @brief Check if any pixel in a tile is nonzero
static BOOL IsTileDirty(
	const UINT8 *biDiff,
	UINT32 imgWidth,
	UINT32 imgHeight,
	UINT32 tileX,
	UINT32 tileY,
	UINT32 tileSize)
{
	UINT32 startX = tileX * tileSize;
	UINT32 startY = tileY * tileSize;
	UINT32 endX = startX + tileSize;
	UINT32 endY = startY + tileSize;
	if (endX > imgWidth) endX = imgWidth;
	if (endY > imgHeight) endY = imgHeight;

	for (UINT32 y = startY; y < endY; ++y)
	{
		for (UINT32 x = startX; x < endX; ++x)
		{
			if (biDiff[y * imgWidth + x] != 0)
				return true;
		}
	}
	return false;
}

// Merge a dirty tile grid into rectangles using a greedy row-span approach:
// 1. For each tile row, find horizontal runs of dirty tiles
// 2. Try to extend each run downward through consecutive tile rows with matching X span
[[nodiscard]] static Result<DirtyRectResult, Error> MergeDirtyTiles(
	const Bitset &dirty,
	UINT32 tilesX,
	UINT32 tilesY,
	UINT32 tileSize,
	UINT32 width,
	UINT32 height)
{
	Vector<DirtyRect> rects;
	if (!rects.Init())
		return Result<DirtyRectResult, Error>::Err(Error::Image_AllocationFailed);

	// visited bit (ty * tilesX + tx) set means this tile is already part of a rect
	Bitset visited;
	if (!visited.Init(tilesX * tilesY))
		return Result<DirtyRectResult, Error>::Err(Error::Image_AllocationFailed);

	for (UINT32 ty = 0; ty < tilesY; ++ty)
	{
		UINT32 tx = 0;
		while (tx < tilesX)
		{
			if (!dirty.Test(ty * tilesX + tx) || visited.Test(ty * tilesX + tx))
			{
				++tx;
				continue;
			}

			// Find the end of the horizontal run of dirty tiles
			UINT32 runStart = tx;
			while (tx < tilesX && dirty.Test(ty * tilesX + tx) && !visited.Test(ty * tilesX + tx))
				++tx;
			UINT32 runEnd = tx; // exclusive

			// Extend downward: check if subsequent rows have the same dirty span
			UINT32 rowEnd = ty + 1;
			while (rowEnd < tilesY)
			{
				BOOL canExtend = true;
				for (UINT32 cx = runStart; cx < runEnd; ++cx)
				{
					if (!dirty.Test(rowEnd * tilesX + cx) || visited.Test(rowEnd * tilesX + cx))
					{
						canExtend = false;
						break;
					}
				}
				if (!canExtend)
					break;
				++rowEnd;
			}

			// Mark all tiles in this rectangle as visited
			for (UINT32 ry = ty; ry < rowEnd; ++ry)
				for (UINT32 cx = runStart; cx < runEnd; ++cx)
					visited.Set(ry * tilesX + cx);

			// Convert tile coordinates to pixel coordinates
			UINT32 pixelX = runStart * tileSize;
			UINT32 pixelY = ty * tileSize;
			UINT32 spanW = (runEnd - runStart) * tileSize;
			UINT32 spanH = (rowEnd - ty) * tileSize;

			// Clamp to image bounds; the JPEG encoder pads partial MCUs
			// internally, so edge-clamped widths are emitted as-is
			UINT32 pixelW = spanW;
			UINT32 pixelH = spanH;
			if (pixelX + pixelW > width) pixelW = width - pixelX;
			if (pixelY + pixelH > height) pixelH = height - pixelY;

			// Filter on the tile-span size, not the clamped size: an edge rect
			// narrower than 32 px still covers a full dirty tile
			if (spanW >= 32 && spanH >= 32)
			{
				DirtyRect rect;
				rect.X = pixelX;
				rect.Y = pixelY;
				rect.Width = pixelW;
				rect.Height = pixelH;
				if (!rects.Add(rect))
					return Result<DirtyRectResult, Error>::Err(Error::Image_AllocationFailed);
			}
		}
	}

	DirtyRectResult result;
	result.Count = (UINT32)rects.Count;
	result.Rects = rects.Release();
	return Result<DirtyRectResult, Error>::Ok(result);
}

[[nodiscard]] Result<DirtyRectResult, Error> ImageProcessor::FindDirtyRects(
	Span<const UINT8> biDiff,
	UINT32 width,
	UINT32 height,
	UINT32 tileSize)
{
	UINT32 tilesX = (width + tileSize - 1) / tileSize;
	UINT32 tilesY = (height + tileSize - 1) / tileSize;

	// Build a dirty tile grid (1 bit per tile instead of a heap BOOL[])
	UINT32 tileCount = tilesX * tilesY;
	Bitset dirty;
	if (!dirty.Init(tileCount))
		return Result<DirtyRectResult, Error>::Err(Error::Image_AllocationFailed);

	for (UINT32 ty = 0; ty < tilesY; ++ty)
		for (UINT32 tx = 0; tx < tilesX; ++tx)
		{
			if (IsTileDirty(biDiff.Data(), width, height, tx, ty, tileSize))
				dirty.Set(ty * tilesX + tx);
		}

	return MergeDirtyTiles(dirty, tilesX, tilesY, tileSize, width, height);
}

[[nodiscard]] Result<DirtyRectResult, Error> ImageProcessor::FindDirtyRects(
	Span<RGB> current,
	Span<const RGB> previous,
	UINT32 width,
	UINT32 height,
	UINT32 tileSize,
	UINT32 threshold)
{
	UINT32 tilesX = (width + tileSize - 1) / tileSize;
	UINT32 tilesY = (height + tileSize - 1) / tileSize;

	// Single pass: scan each tile's pixels inline and stop at the first one
	// past the threshold — no bidiff map is materialized
	Bitset dirty;
	if (!dirty.Init(tilesX * tilesY))
		return Result<DirtyRectResult, Error>::Err(Error::Image_AllocationFailed);

	for (UINT32 ty = 0; ty < tilesY; ++ty)
	{
		UINT32 startY = ty * tileSize;
		UINT32 endY = startY + tileSize;
		if (endY > height) endY = height;

		for (UINT32 tx = 0; tx < tilesX; ++tx)
		{
			UINT32 startX = tx * tileSize;
			UINT32 endX = startX + tileSize;
			if (endX > width) endX = width;

			BOOL tileDirty = false;
			BOOL anyDrift = false;
			for (UINT32 y = startY; y < endY && !tileDirty; ++y)
			{
				const RGB *cur = current.Data() + (USIZE)y * width + startX;
				const RGB *prev = previous.Data() + (USIZE)y * width + startX;
				const UINT8 *cur8 = (const UINT8 *)cur;
				const UINT8 *prev8 = (const UINT8 *)prev;
				UINT32 rowPixels = endX - startX;

				UINT32 x = 0;
#if defined(ARCHITECTURE_X86_64)
				// 16-px chunks are exactly 48 bytes per side, three 16-byte
				// loads; the loop condition x + 16 <= rowPixels keeps every
				// load inside the row
				for (; x + 16 <= rowPixels; x += 16, cur8 += 48, prev8 += 48)
				{
					SseVec16b d0 = SseLoadU(cur8) ^ SseLoadU(prev8);
					SseVec16b d1 = SseLoadU(cur8 + 16) ^ SseLoadU(prev8 + 16);
					SseVec16b d2 = SseLoadU(cur8 + 32) ^ SseLoadU(prev8 + 32);
					SseVec16b d = d0 | (d1 | d2);

					// All 48 bytes equal: 16 pixels of SAD 0 — no drift, no
					// dirty, and the per-pixel SAD loop is skipped entirely
					if (SseAllZero(d))
						continue;

					if (ScanPixelsScalar(cur8, prev8, 16, threshold, anyDrift))
					{
						tileDirty = true;
						break;
					}
				}
#endif
				// Row tail (and the whole row on non-x86_64): the helper's
				// final pixel is the row's last, read as RGB only
				if (!tileDirty && x < rowPixels)
				{
					if (ScanPixelsScalar(cur8, prev8, rowPixels - x, threshold, anyDrift))
						tileDirty = true;
				}
			}

			if (tileDirty)
			{
				dirty.Set(ty * tilesX + tx);
			}
			else if (anyDrift)
			{
				// Revert the sub-threshold drift so the diff base stays what
				// the receiver has — the next capture re-diffs the accumulated
				// change against the same base until it crosses the threshold
				for (UINT32 y = startY; y < endY; ++y)
				{
					RGB *dst = current.Data() + (USIZE)y * width + startX;
					const RGB *src = previous.Data() + (USIZE)y * width + startX;
					Memory::Copy(dst, src, (USIZE)(endX - startX) * sizeof(RGB));
				}
			}
		}
	}

	return MergeDirtyTiles(dirty, tilesX, tilesY, tileSize, width, height);
}
