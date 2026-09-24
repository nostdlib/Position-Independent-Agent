#pragma once

#include "lib/runtime.h"
#include "tests.h"

class JpegTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running JPEG Encoder Tests...");

		// Validation tests
		RunTest(allPassed, &TestInvalidComponents_Returns_Error, "JPEG reject invalid component count");
		RunTest(allPassed, &TestZeroWidth_Returns_Error, "JPEG reject zero width");
		RunTest(allPassed, &TestZeroHeight_Returns_Error, "JPEG reject zero height");
		RunTest(allPassed, &TestNegativeWidth_Returns_Error, "JPEG reject negative width");
		RunTest(allPassed, &TestNegativeHeight_Returns_Error, "JPEG reject negative height");
		RunTest(allPassed, &TestOversizedWidth_Returns_Error, "JPEG reject oversized width");
		RunTest(allPassed, &TestOversizedHeight_Returns_Error, "JPEG reject oversized height");

		// Encoding tests
		RunTest(allPassed, &TestEncode1x1_RGB, "JPEG encode 1x1 RGB");
		RunTest(allPassed, &TestEncode1x1_RGBA, "JPEG encode 1x1 RGBA");
		RunTest(allPassed, &TestEncode8x8_RGB, "JPEG encode 8x8 RGB");
		RunTest(allPassed, &TestEncodeNonMultipleOf8, "JPEG encode non-multiple-of-8 dimensions");
		RunTest(allPassed, &TestEncodeQualityBounds, "JPEG encode with clamped quality");
		RunTest(allPassed, &TestEncodeAllBlack, "JPEG encode all-black image");
		RunTest(allPassed, &TestEncodeAllWhite, "JPEG encode all-white image");
		RunTest(allPassed, &TestSOF0SamplingQualityGate, "JPEG SOF0 sampling quality gate (4:2:0 below q90)");
		RunTest(allPassed, &TestEncodeSubsampledEdgeSizes, "JPEG encode 4:2:0 partial-MCU edge sizes");

		if (allPassed)
			LOG_INFO("All JPEG tests passed!");
		else
			LOG_ERROR("Some JPEG tests failed!");

		return allPassed;
	}

private:
	// Captured output buffer for test verification
	struct CaptureBuffer
	{
		UINT8 data[16384];
		USIZE size;
	};

	static VOID CaptureCallback(PVOID context, PVOID data, INT32 size)
	{
		auto *buf = (CaptureBuffer *)context;
		if (buf->size + (USIZE)size <= sizeof(buf->data))
		{
			Memory::Copy(buf->data + buf->size, data, (USIZE)size);
			buf->size += (USIZE)size;
		}
	}

	// Verify SOI (0xFFD8) at start and EOI (0xFFD9) at end
	static BOOL VerifyJpegMarkers(const CaptureBuffer &buf)
	{
		if (buf.size < 4)
		{
			LOG_ERROR("JPEG output too small: %u bytes", (UINT32)buf.size);
			return false;
		}
		if (buf.data[0] != 0xFF || buf.data[1] != 0xD8)
		{
			LOG_ERROR("Missing SOI marker: got 0x%02X%02X", buf.data[0], buf.data[1]);
			return false;
		}
		if (buf.data[buf.size - 2] != 0xFF || buf.data[buf.size - 1] != 0xD9)
		{
			LOG_ERROR("Missing EOI marker: got 0x%02X%02X", buf.data[buf.size - 2], buf.data[buf.size - 1]);
			return false;
		}
		return true;
	}

	// --- Validation tests ---

	static BOOL TestInvalidComponents_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 128; // B
		pixel[1] = 128; // G
		pixel[2] = 128; // R
		CaptureBuffer buf;
		buf.size = 0;

		auto r2 = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 1, 1, 2, Span<const UINT8>(pixel, 3));
		if (r2)
		{
			LOG_ERROR("numComponents=2 should fail");
			return false;
		}

		auto r5 = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 1, 1, 5, Span<const UINT8>(pixel, 3));
		if (r5)
		{
			LOG_ERROR("numComponents=5 should fail");
			return false;
		}

		return true;
	}

	static BOOL TestZeroWidth_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 0; // B
		pixel[1] = 0; // G
		pixel[2] = 0; // R
		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 0, 1, 3, Span<const UINT8>(pixel, 3));
		return !r;
	}

	static BOOL TestZeroHeight_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 0; // B
		pixel[1] = 0; // G
		pixel[2] = 0; // R
		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 1, 0, 3, Span<const UINT8>(pixel, 3));
		return !r;
	}

	static BOOL TestNegativeWidth_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 0; // B
		pixel[1] = 0; // G
		pixel[2] = 0; // R
		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, -1, 1, 3, Span<const UINT8>(pixel, 3));
		return !r;
	}

	static BOOL TestNegativeHeight_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 0; // B
		pixel[1] = 0; // G
		pixel[2] = 0; // R
		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 1, -1, 3, Span<const UINT8>(pixel, 3));
		return !r;
	}

	static BOOL TestOversizedWidth_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 0; // B
		pixel[1] = 0; // G
		pixel[2] = 0; // R
		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 0x10000, 1, 3, Span<const UINT8>(pixel, 3));
		return !r;
	}

	static BOOL TestOversizedHeight_Returns_Error()
	{
		UINT8 pixel[3];
		pixel[0] = 0; // B
		pixel[1] = 0; // G
		pixel[2] = 0; // R
		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, 1, 0x10000, 3, Span<const UINT8>(pixel, 3));
		return !r;
	}

	// --- Encoding tests ---

	static BOOL TestEncode1x1_RGB()
	{
		UINT8 pixel[3];
		pixel[0] = 255; // B
		pixel[1] = 0;	// G
		pixel[2] = 0;	// R
		CaptureBuffer buf;
		buf.size = 0;

		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 1, 1, 3, Span<const UINT8>(pixel, 3));
		if (!r)
		{
			LOG_ERROR("Encode 1x1 RGB failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	static BOOL TestEncode1x1_RGBA()
	{
		UINT8 pixel[4];
		pixel[0] = 0;	// B
		pixel[1] = 255; // G
		pixel[2] = 0;	// R
		pixel[3] = 255; // A
		CaptureBuffer buf;
		buf.size = 0;

		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 1, 1, 4, Span<const UINT8>(pixel, 4));
		if (!r)
		{
			LOG_ERROR("Encode 1x1 RGBA failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	static BOOL TestEncode8x8_RGB()
	{
		// 8x8 gradient image
		UINT8 pixels[8 * 8 * 3];
		for (INT32 y = 0; y < 8; ++y)
		{
			for (INT32 x = 0; x < 8; ++x)
			{
				INT32 i = (y * 8 + x) * 3;
				pixels[i + 0] = (UINT8)(x * 32); // B
				pixels[i + 1] = (UINT8)(y * 32); // G
				pixels[i + 2] = (UINT8)(128);	 // R
			}
		}

		CaptureBuffer buf;
		buf.size = 0;

		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 90, 8, 8, 3, Span<const UINT8>(pixels, sizeof(pixels)));
		if (!r)
		{
			LOG_ERROR("Encode 8x8 RGB failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	static BOOL TestEncodeNonMultipleOf8()
	{
		// 3x5 image — tests edge-block clamping
		constexpr INT32 w = 3;
		constexpr INT32 h = 5;
		UINT8 pixels[w * h * 3];
		for (INT32 i = 0; i < w * h * 3; ++i)
			pixels[i] = (UINT8)(i & 0xFF);

		CaptureBuffer buf;
		buf.size = 0;

		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 50, w, h, 3, Span<const UINT8>(pixels, sizeof(pixels)));
		if (!r)
		{
			LOG_ERROR("Encode 3x5 failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	static BOOL TestEncodeQualityBounds()
	{
		UINT8 pixel[3];
		pixel[0] = 100; // B
		pixel[1] = 150; // G
		pixel[2] = 200; // R
		CaptureBuffer buf;

		// Quality 0 (clamped to 1)
		buf.size = 0;
		auto r1 = JpegEncoder::Encode(&CaptureCallback, &buf, 0, 1, 1, 3, Span<const UINT8>(pixel, 3));
		if (!r1)
		{
			LOG_ERROR("Quality 0 failed: %e", r1.Error());
			return false;
		}
		if (!VerifyJpegMarkers(buf))
			return false;

		// Quality 200 (clamped to 100)
		buf.size = 0;
		auto r2 = JpegEncoder::Encode(&CaptureCallback, &buf, 200, 1, 1, 3, Span<const UINT8>(pixel, 3));
		if (!r2)
		{
			LOG_ERROR("Quality 200 failed: %e", r2.Error());
			return false;
		}
		if (!VerifyJpegMarkers(buf))
			return false;

		return true;
	}

	static BOOL TestEncodeAllBlack()
	{
		UINT8 pixels[8 * 8 * 3];
		Memory::Zero(pixels, sizeof(pixels));

		CaptureBuffer buf;
		buf.size = 0;

		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 8, 8, 3, Span<const UINT8>(pixels, sizeof(pixels)));
		if (!r)
		{
			LOG_ERROR("Encode all-black failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	static BOOL TestEncodeAllWhite()
	{
		UINT8 pixels[8 * 8 * 3];
		Memory::Set(pixels, 0xFF, sizeof(pixels));

		CaptureBuffer buf;
		buf.size = 0;

		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 8, 8, 3, Span<const UINT8>(pixels, sizeof(pixels)));
		if (!r)
		{
			LOG_ERROR("Encode all-white failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	// Find the SOF0 marker and return the three components' sampling factors
	static BOOL ReadSOF0Sampling(const CaptureBuffer &buf, UINT8 sampling[3])
	{
		for (USIZE i = 0; i + 19 <= buf.size; i++)
		{
			if (buf.data[i] == 0xFF && buf.data[i + 1] == 0xC0)
			{
				if (buf.data[i + 9] != 3)
					return false;
				for (INT32 c = 0; c < 3; c++)
					sampling[c] = buf.data[i + 10 + c * 3 + 1];
				return true;
			}
		}
		return false;
	}

	// Encode a deterministic gradient at the given quality/size and read back
	// the SOF0 sampling factors
	static BOOL EncodeAndReadSampling(INT32 quality, INT32 width, INT32 height, UINT8 sampling[3])
	{
		UINT8 pixels[64 * 64 * 3];
		for (INT32 i = 0; i < width * height * 3; ++i)
			pixels[i] = (UINT8)((i * 7 + (i >> 4) * 13) & 0xFF);

		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, quality, width, height, 3,
		                             Span<const UINT8>(pixels, (USIZE)width * height * 3));
		if (!r)
		{
			LOG_ERROR("Encode %dx%d q%d failed: %e", width, height, quality, r.Error());
			return false;
		}
		if (!VerifyJpegMarkers(buf))
			return false;
		return ReadSOF0Sampling(buf, sampling);
	}

	// Quality gate: below 90 encodes 4:2:0 (luma 0x22, chroma 0x11);
	// 90 and above keep the original 4:4:4 (all 0x11)
	static BOOL TestSOF0SamplingQualityGate()
	{
		struct
		{
			INT32 quality;
			UINT8 expected[3];
		} cases[4] = {
			{75, {0x22, 0x11, 0x11}},
			{89, {0x22, 0x11, 0x11}},
			{90, {0x11, 0x11, 0x11}},
			{95, {0x11, 0x11, 0x11}}};

		for (UINT32 c = 0; c < 4; c++)
		{
			UINT8 s[3];
			if (!EncodeAndReadSampling(cases[c].quality, 64, 64, s) ||
			    s[0] != cases[c].expected[0] || s[1] != cases[c].expected[1] || s[2] != cases[c].expected[2])
			{
				LOG_ERROR("q%d sampling: expected %02X/%02X/%02X, got %02X/%02X/%02X",
				          cases[c].quality, cases[c].expected[0], cases[c].expected[1], cases[c].expected[2],
				          s[0], s[1], s[2]);
				return false;
			}
		}
		return true;
	}

	// 4:2:0 MCU edge clamping: sizes that force partial 16x16 MCUs
	static BOOL TestEncodeSubsampledEdgeSizes()
	{
		const INT32 sizes[4][2] = {{1, 1}, {13, 7}, {16, 16}, {17, 33}};
		for (INT32 c = 0; c < 4; c++)
		{
			UINT8 pixels[17 * 33 * 3];
			for (INT32 i = 0; i < sizes[c][0] * sizes[c][1] * 3; ++i)
				pixels[i] = (UINT8)((i * 11 + (i >> 3)) & 0xFF);

			CaptureBuffer buf;
			buf.size = 0;
			auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, sizes[c][0], sizes[c][1], 3,
			                             Span<const UINT8>(pixels, (USIZE)sizes[c][0] * sizes[c][1] * 3));
			if (!r)
			{
				LOG_ERROR("Encode %dx%d q75 failed: %e", sizes[c][0], sizes[c][1], r.Error());
				return false;
			}
			if (!VerifyJpegMarkers(buf))
				return false;
		}
		return true;
	}
};
