/**
 * @file jpeg_encoder.h
 * @brief Position-Independent JPEG Encoder
 *
 * @details Implements a baseline JPEG encoder (ITU-T T.81) that compresses
 * raw RGB/RGBA image data into JFIF-compliant JPEG output. The encoder uses
 * the Arai–Agui–Nakajima scaled DCT algorithm and standard Huffman tables
 * from JPEG Annex K.
 *
 * Output is streamed through a user-provided callback, avoiding the need
 * for a large contiguous output buffer.
 *
 * @note Based on Tiny JPEG Encoder by Sergio Gonzalez, adapted for
 * position-independent execution (no .rdata, no CRT).
 *
 * @see ITU-T T.81 — Digital compression and coding of continuous-tone
 *      still images — Requirements and guidelines (JPEG standard)
 *      https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 * @see JFIF 1.02 — JPEG File Interchange Format
 *      https://www.w3.org/Graphics/JPEG/jfif3.pdf
 *
 * @ingroup runtime
 *
 * @defgroup jpeg JPEG Encoder
 * @ingroup runtime
 * @{
 */

#pragma once

#include "platform/platform.h"

/**
 * @brief Write callback invoked by JpegEncoder to emit compressed data
 *
 * @param context User-provided opaque pointer (e.g., file handle, buffer)
 * @param data Pointer to the compressed JPEG bytes to write
 * @param size Number of bytes to write
 */
using JpegWriteFunc = VOID(PVOID context, PVOID data, INT32 size);

/**
 * @class JpegEncoder
 * @brief Baseline JPEG encoder for RGB/RGBA images
 *
 * @details Encodes raw pixel data into baseline JPEG (ITU-T T.81) format.
 * Supports 3-component (RGB) and 4-component (RGBA, alpha discarded) input.
 * Quality ranges from 1 (lowest) to 100 (highest), controlling quantization
 * table scaling per JPEG Annex K.
 *
 * All encoding is performed statelessly through a single static method.
 * Output is streamed to a user-provided callback function.
 *
 * @par Example Usage:
 * @code
 * UINT8 pixels[width * height * 3]; // RGB data
 * auto result = JpegEncoder::Encode(
 *     myWriteFunc, myContext, 85,
 *     width, height, 3,
 *     Span<const UINT8>(pixels, width * height * 3));
 * if (!result)
 *     LOG_ERROR("JPEG encode failed: %e", result.Error());
 * @endcode
 *
 * @see ITU-T T.81 Section 4 — Modes of operation
 *      https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 */
class JpegEncoder
{
public:
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;

	/**
	 * @brief Encode raw pixel data to JPEG format
	 *
	 * @details Performs baseline JPEG encoding using 8x8 DCT blocks with
	 * standard Huffman tables (JPEG Annex K.3) and quality-scaled quantization
	 * tables (JPEG Annex K.1). The compressed output is streamed to the
	 * provided write callback in buffered chunks.
	 *
	 * @param func Write callback for emitting compressed JPEG data
	 * @param context Opaque pointer passed through to the write callback
	 * @param quality Compression quality (1–100, clamped; 100 = best quality)
	 * @param width Image width in pixels (must be 1–65535)
	 * @param height Image height in pixels (must be 1–65535)
	 * @param numComponents Bytes per pixel (3 = RGB, 4 = RGBA)
	 * @param srcData Raw pixel data in row-major order, left-to-right, top-to-bottom
	 * @return Ok on success, or Jpeg_InvalidParams if parameters are out of range
	 *
	 * @see ITU-T T.81 Annex K — Examples and guidelines
	 *      https://www.w3.org/Graphics/JPEG/itu-t81.pdf
	 */
	[[nodiscard]] static Result<void, Error> Encode(
		JpegWriteFunc *func,
		PVOID context,
		INT32 quality,
		INT32 width,
		INT32 height,
		INT32 numComponents,
		Span<const UINT8> srcData);

	/**
	 * @brief Encode a sub-rectangle of a larger frame to JPEG format
	 *
	 * @details Identical to the packed-row overload except rows are read
	 * stride pixels apart instead of width pixels apart, so a dirty
	 * rectangle can be encoded in place straight out of the frame buffer
	 * (srcData points at the rectangle's top-left pixel) with no row
	 * gather into a staging buffer.
	 *
	 * @param func Write callback for emitting compressed JPEG data
	 * @param context Opaque pointer passed through to the write callback
	 * @param quality Compression quality (1–100, clamped; 100 = best quality)
	 * @param width Rectangle width in pixels (must be 1–65535)
	 * @param height Rectangle height in pixels (must be 1–65535)
	 * @param numComponents Bytes per pixel (3 = RGB, 4 = RGBA)
	 * @param srcData Raw pixel data starting at the rectangle's top-left
	 *                pixel; must cover at least (stride * (height - 1) + width)
	 *                pixels of the enclosing frame
	 * @param stride Row pitch of the enclosing frame in pixels (>= width)
	 * @return Ok on success, or Jpeg_InvalidParams if parameters are out of range
	 */
	[[nodiscard]] static Result<void, Error> Encode(
		JpegWriteFunc *func,
		PVOID context,
		INT32 quality,
		INT32 width,
		INT32 height,
		INT32 numComponents,
		Span<const UINT8> srcData,
		INT32 stride);
};

/** @} */ // end of jpeg group
