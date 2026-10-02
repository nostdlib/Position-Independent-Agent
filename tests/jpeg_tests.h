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

		// Quality clamp edges observed through the emitted tables and bytes
		RunTest(allPassed, &TestEncodeQualityClampEquivalence, "JPEG quality clamp: q0==q1 and q200==q100 byte-for-byte");
		RunTest(allPassed, &TestEncodeQualityDQTScale, "JPEG DQT luma scale pins the clamped quality");
		RunTest(allPassed, &TestSOF0SamplingClampedQuality, "JPEG SOF0 sampling at clamped quality extremes");

		// Tiny images and the stride (sub-rect) overload
		RunTest(allPassed, &TestEncode7x5AcrossGate, "JPEG encode 7x5 across the 4:2:0 gate boundary");
		RunTest(allPassed, &TestEncodeStrideMatchesPacked, "JPEG stride encode of a sub-rect matches packed rows");
		RunTest(allPassed, &TestEncodeStride1x1Rect, "JPEG stride encode of a 1x1 corner rect");
		RunTest(allPassed, &TestEncodeRepeatedDeterministic, "JPEG repeated encodes stay byte-identical");
		RunTest(allPassed, &TestEncodeRgbaMatchesRgb, "JPEG RGBA SIMD path matches the RGB path byte-for-byte");
		RunTest(allPassed, &TestEncodePureRedDC, "JPEG pure-red DC differentials pin pixel content");

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

	// Find the SOF0 marker and return the frame dimensions it declares
	// ([FF][C0][len u16][precision][height u16][width u16][ncomp])
	static BOOL ReadSOF0Dimensions(const CaptureBuffer &buf, INT32 &width, INT32 &height)
	{
		for (USIZE i = 0; i + 9 <= buf.size; i++)
		{
			if (buf.data[i] == 0xFF && buf.data[i + 1] == 0xC0)
			{
				height = (INT32)((UINT32)buf.data[i + 5] << 8 | buf.data[i + 6]);
				width = (INT32)((UINT32)buf.data[i + 7] << 8 | buf.data[i + 8]);
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
			INT32 sofWidth = 0;
			INT32 sofHeight = 0;
			if (!ReadSOF0Dimensions(buf, sofWidth, sofHeight) || sofWidth != sizes[c][0] || sofHeight != sizes[c][1])
			{
				LOG_ERROR("SOF0 of %dx%d declares %dx%d", sizes[c][0], sizes[c][1], sofWidth, sofHeight);
				return false;
			}
		}
		return true;
	}

	// --- Quality clamp edge tests ---

	// Encode a deterministic pattern (up to 64x64) into a capture buffer
	static BOOL EncodePatternInto(CaptureBuffer &buf, INT32 quality, INT32 width, INT32 height)
	{
		UINT8 pixels[64 * 64 * 3];
		for (INT32 i = 0; i < width * height * 3; ++i)
			pixels[i] = (UINT8)((i * 7 + (i >> 4) * 13) & 0xFF);

		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, quality, width, height, 3,
		                             Span<const UINT8>(pixels, (USIZE)width * height * 3));
		if (!r)
		{
			LOG_ERROR("Encode %dx%d q%d failed: %e", width, height, quality, r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	// Find the first DQT marker and return its first (DC) luma table byte
	static BOOL ReadFirstDQTValue(const CaptureBuffer &buf, UINT8 &value)
	{
		for (USIZE i = 0; i + 6 <= buf.size; i++)
		{
			if (buf.data[i] == 0xFF && buf.data[i + 1] == 0xDB)
			{
				value = buf.data[i + 5];
				return true;
			}
		}
		return false;
	}

	// Clamping happens before table scaling: out-of-range qualities must
	// produce the exact bytes of their clamped values
	static BOOL TestEncodeQualityClampEquivalence()
	{
		CaptureBuffer a;
		CaptureBuffer b;

		if (!EncodePatternInto(a, 0, 16, 16) || !EncodePatternInto(b, 1, 16, 16))
			return false;
		if (!CompareBytes(Span<const UINT8>(a.data, a.size), Span<const UINT8>(b.data, b.size)))
		{
			LOG_ERROR("q0 output differs from q1 (%u vs %u bytes)", (UINT32)a.size, (UINT32)b.size);
			return false;
		}

		if (!EncodePatternInto(a, 200, 16, 16) || !EncodePatternInto(b, 100, 16, 16))
			return false;
		if (!CompareBytes(Span<const UINT8>(a.data, a.size), Span<const UINT8>(b.data, b.size)))
		{
			LOG_ERROR("q200 output differs from q100 (%u vs %u bytes)", (UINT32)a.size, (UINT32)b.size);
			return false;
		}
		return true;
	}

	// IJG table scaling at the clamp edges: q1 saturates at 255, q100 floors
	// at 1, q25/q50 land on hand-computed midpoints
	static BOOL TestEncodeQualityDQTScale()
	{
		struct
		{
			INT32 quality;
			UINT8 expected;
		} cases[6] = {{0, 255}, {1, 255}, {25, 32}, {50, 16}, {100, 1}, {200, 1}};

		for (UINT32 c = 0; c < 6; c++)
		{
			CaptureBuffer buf;
			UINT8 value = 0;
			if (!EncodePatternInto(buf, cases[c].quality, 16, 16) ||
			    !ReadFirstDQTValue(buf, value) || value != cases[c].expected)
			{
				LOG_ERROR("q%d DQT luma DC: expected %u, got %u",
				          cases[c].quality, cases[c].expected, value);
				return false;
			}
		}
		return true;
	}

	// The 4:2:0 gate sees the clamped quality: q0 subsamples, q200 stays 4:4:4
	static BOOL TestSOF0SamplingClampedQuality()
	{
		struct
		{
			INT32 quality;
			UINT8 expected[3];
		} cases[5] = {
			{0, {0x22, 0x11, 0x11}},
			{1, {0x22, 0x11, 0x11}},
			{100, {0x11, 0x11, 0x11}},
			{200, {0x11, 0x11, 0x11}},
			{1000, {0x11, 0x11, 0x11}}};

		for (UINT32 c = 0; c < 5; c++)
		{
			UINT8 s[3] = {0, 0, 0};
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

	// --- Tiny images and the stride overload ---

	// 7x5 forces partial MCUs on both axes; the sampling gate must still
	// switch at q89/q90 at this size
	static BOOL TestEncode7x5AcrossGate()
	{
		UINT8 s89[3] = {0, 0, 0};
		UINT8 s90[3] = {0, 0, 0};
		if (!EncodeAndReadSampling(89, 7, 5, s89) || !EncodeAndReadSampling(90, 7, 5, s90))
			return false;
		if (s89[0] != 0x22 || s89[1] != 0x11 || s89[2] != 0x11)
		{
			LOG_ERROR("7x5 q89 sampling: got %02X/%02X/%02X, want 22/11/11", s89[0], s89[1], s89[2]);
			return false;
		}
		if (s90[0] != 0x11 || s90[1] != 0x11 || s90[2] != 0x11)
		{
			LOG_ERROR("7x5 q90 sampling: got %02X/%02X/%02X, want 11/11/11", s90[0], s90[1], s90[2]);
			return false;
		}
		return true;
	}

	// A 45x27 sub-rect (width not a multiple of 8) read from a 130x33 frame
	// with stride must match a packed encode of the gathered rows byte-for-byte
	static BOOL TestEncodeStrideMatchesPacked()
	{
		constexpr INT32 fw = 130;
		constexpr INT32 fh = 33;
		constexpr INT32 rw = 45;
		constexpr INT32 rh = 27;
		constexpr INT32 ox = 7;
		constexpr INT32 oy = 5;

		UINT8 frame[fw * fh * 3];
		for (INT32 i = 0; i < fw * fh * 3; ++i)
			frame[i] = (UINT8)((i * 7 + (i >> 4) * 13) & 0xFF);

		UINT8 packed[rw * rh * 3];
		for (INT32 y = 0; y < rh; ++y)
			Memory::Copy(packed + (USIZE)y * rw * 3,
			             frame + ((USIZE)(oy + y) * fw + ox) * 3, (USIZE)rw * 3);

		CaptureBuffer strideBuf;
		strideBuf.size = 0;
		CaptureBuffer packedBuf;
		packedBuf.size = 0;

		USIZE offset = ((USIZE)oy * fw + ox) * 3;
		auto rs = JpegEncoder::Encode(&CaptureCallback, &strideBuf, 60, rw, rh, 3,
		                              Span<const UINT8>(frame + offset, sizeof(frame) - offset), fw);
		auto rp = JpegEncoder::Encode(&CaptureCallback, &packedBuf, 60, rw, rh, 3,
		                              Span<const UINT8>(packed, sizeof(packed)));
		if (!rs)
		{
			LOG_ERROR("Stride encode %dx%d failed: %e", rw, rh, rs.Error());
			return false;
		}
		if (!rp)
		{
			LOG_ERROR("Packed encode %dx%d failed: %e", rw, rh, rp.Error());
			return false;
		}
		return CompareBytes(Span<const UINT8>(strideBuf.data, strideBuf.size),
		                    Span<const UINT8>(packedBuf.data, packedBuf.size));
	}

	// 1x1 rectangles pulled from the corners of a larger strided frame
	static BOOL TestEncodeStride1x1Rect()
	{
		constexpr INT32 fw = 40;
		constexpr INT32 fh = 20;
		UINT8 frame[fw * fh * 3];
		for (INT32 i = 0; i < fw * fh * 3; ++i)
			frame[i] = (UINT8)((i * 9 + (i >> 5)) & 0xFF);

		// Bottom-right corner pixel (the last byte triple of the frame)
		CaptureBuffer buf;
		buf.size = 0;
		USIZE offset = ((USIZE)(fh - 1) * fw + (fw - 1)) * 3;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 1, 1, 3,
		                             Span<const UINT8>(frame + offset, 3), fw);
		if (!r)
		{
			LOG_ERROR("Stride encode of the corner 1x1 rect failed: %e", r.Error());
			return false;
		}
		if (!VerifyJpegMarkers(buf))
			return false;

		// Top-left pixel with a stride larger than the rect
		buf.size = 0;
		r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 1, 1, 3,
		                        Span<const UINT8>(frame, 3), fw);
		if (!r)
		{
			LOG_ERROR("Stride encode of the origin 1x1 rect failed: %e", r.Error());
			return false;
		}
		return VerifyJpegMarkers(buf);
	}

	// The same input encoded twice with a different encode in between must
	// produce identical bytes — no state may carry between Encode calls
	static BOOL TestEncodeRepeatedDeterministic()
	{
		CaptureBuffer first;
		CaptureBuffer second;

		if (!EncodePatternInto(first, 70, 32, 24))
			return false;
		{
			// Different size and quality in between
			CaptureBuffer middle;
			if (!EncodePatternInto(middle, 90, 16, 16))
				return false;
		}
		if (!EncodePatternInto(second, 70, 32, 24))
			return false;

		return CompareBytes(Span<const UINT8>(first.data, first.size),
		                    Span<const UINT8>(second.data, second.size));
	}

	// RGBA frames must encode to the exact bytes of the RGB frames they derive
	// from: DeinterleaveRgba4 reads only the R/G/B lanes, and at 32 wide every
	// MCU takes the SIMD loader (it requires mcuX + 16 <= width)
	static BOOL TestEncodeRgbaMatchesRgb()
	{
		constexpr INT32 w = 32;
		constexpr INT32 h = 32;
		UINT8 rgb[w * h * 3];
		for (INT32 i = 0; i < w * h * 3; ++i)
			rgb[i] = (UINT8)((i * 7 + (i >> 4) * 13) & 0xFF);

		UINT8 rgba[w * h * 4];
		for (INT32 p = 0; p < w * h; ++p)
		{
			rgba[p * 4 + 0] = rgb[p * 3 + 0];
			rgba[p * 4 + 1] = rgb[p * 3 + 1];
			rgba[p * 4 + 2] = rgb[p * 3 + 2];
			rgba[p * 4 + 3] = 0x80; // constant alpha, ignored by the deinterleave
		}

		CaptureBuffer rgbBuf;
		rgbBuf.size = 0;
		CaptureBuffer rgbaBuf;
		rgbaBuf.size = 0;

		auto r3 = JpegEncoder::Encode(&CaptureCallback, &rgbBuf, 75, w, h, 3, Span<const UINT8>(rgb, sizeof(rgb)));
		auto r4 = JpegEncoder::Encode(&CaptureCallback, &rgbaBuf, 75, w, h, 4, Span<const UINT8>(rgba, sizeof(rgba)));
		if (!r3)
		{
			LOG_ERROR("RGB encode %dx%d failed: %e", w, h, r3.Error());
			return false;
		}
		if (!r4)
		{
			LOG_ERROR("RGBA encode %dx%d failed: %e", w, h, r4.Error());
			return false;
		}
		if (rgbBuf.size != rgbaBuf.size)
		{
			LOG_ERROR("RGBA stream is %u bytes, RGB is %u", (UINT32)rgbaBuf.size, (UINT32)rgbBuf.size);
			return false;
		}
		return CompareBytes(Span<const UINT8>(rgbBuf.data, rgbBuf.size),
		                    Span<const UINT8>(rgbaBuf.data, rgbaBuf.size));
	}

	// --- Minimal scan decoder for the content-level pin (DC differentials) ---

	// One canonical Huffman table entry: `code` of `len` bits decodes to `sym`
	struct HuffEntry
	{
		UINT32 code;
		UINT8 len;
		UINT8 sym;
	};

	struct MiniHuffTable
	{
		HuffEntry entries[256];
		UINT32 count;
	};

	// Canonical code assignment per T.81 F.2.2.3, from a DHT BITS/HUFFVAL pair
	static VOID BuildMiniHuff(const UINT8 *bits, const UINT8 *vals, MiniHuffTable &table)
	{
		table.count = 0;
		UINT32 code = 0;
		UINT32 k = 0;
		for (UINT32 len = 1; len <= 16; len++)
		{
			for (UINT32 b = 0; b < bits[len - 1]; b++)
			{
				table.entries[table.count].code = code;
				table.entries[table.count].len = (UINT8)len;
				table.entries[table.count].sym = vals[k];
				table.count++;
				code++;
				k++;
			}
			code <<= 1;
		}
	}

	// MSB-first bit cursor over the entropy-coded scan (0xFF00 destuffing)
	struct ScanReader
	{
		const UINT8 *data;
		USIZE size;
		USIZE byte;
		UINT32 bit;
	};

	static UINT32 NextScanBit(ScanReader &r)
	{
		if (r.bit == 0 && r.data[r.byte] == 0xFF)
			r.byte++; // skip the stuffed 0x00
		UINT32 v = (UINT32)(r.data[r.byte] >> (7 - r.bit)) & 1;
		if (++r.bit == 8)
		{
			r.bit = 0;
			r.byte++;
		}
		return v;
	}

	static UINT8 DecodeHuffSymbol(ScanReader &r, const MiniHuffTable &table)
	{
		UINT32 cur = 0;
		for (UINT32 len = 1; len <= 16; len++)
		{
			cur = (cur << 1) | NextScanBit(r);
			for (UINT32 i = 0; i < table.count; i++)
				if (table.entries[i].len == len && table.entries[i].code == cur)
					return table.entries[i].sym;
		}
		return 0xFF; // not a valid symbol
	}

	static INT32 ReadScanVli(ScanReader &r, UINT32 bits)
	{
		if (bits == 0 || bits > 15)
			return 0;
		UINT32 v = 0;
		for (UINT32 i = 0; i < bits; i++)
			v = (v << 1) | NextScanBit(r);
		if (v < (1u << (bits - 1)))
			return (INT32)v - (INT32)((1u << bits) - 1u);
		return (INT32)v;
	}

	// Decode one block's DC differential, skipping its AC run-length tail
	static INT32 DecodeBlockDC(ScanReader &r, const MiniHuffTable &dc, const MiniHuffTable &ac)
	{
		INT32 diff = ReadScanVli(r, DecodeHuffSymbol(r, dc));
		for (;;)
		{
			UINT8 sym = DecodeHuffSymbol(r, ac);
			if (sym == 0x00 || sym == 0xFF) // EOB (or invalid)
				break;
			if (sym != 0xF0) // ZRL carries no extra bits
				ReadScanVli(r, sym & 0x0F);
		}
		return diff;
	}

	// First content-level pin: pure red must land in the stream as exact DC
	// differentials — a swapped R/B lane or permuted MCU quadrant would shift
	// them. Nothing else in this suite asserts pixel content.
	static BOOL TestEncodePureRedDC()
	{
		// 16x16 pure red; the encoder reads px[0] as R
		UINT8 pixels[16 * 16 * 3];
		for (INT32 i = 0; i < 16 * 16; ++i)
		{
			pixels[i * 3 + 0] = 255; // R
			pixels[i * 3 + 1] = 0;	  // G
			pixels[i * 3 + 2] = 0;	  // B
		}

		CaptureBuffer buf;
		buf.size = 0;
		auto r = JpegEncoder::Encode(&CaptureCallback, &buf, 75, 16, 16, 3,
		                             Span<const UINT8>(pixels, sizeof(pixels)));
		if (!r)
		{
			LOG_ERROR("Pure-red encode failed: %e", r.Error());
			return false;
		}

		// Walk the markers: collect the four DHT tables, then let the SOS
		// segment's length prefix locate the start of the scan data
		MiniHuffTable dcLuma = {};
		MiniHuffTable acLuma = {};
		MiniHuffTable dcChroma = {};
		MiniHuffTable acChroma = {};
		USIZE scanStart = 0;
		USIZE i = 2; // skip SOI
		while (i + 4 <= buf.size)
		{
			if (buf.data[i] != 0xFF)
				break;
			UINT8 marker = buf.data[i + 1];
			if (marker == 0xD9)
				break;
			UINT32 len = (UINT32)buf.data[i + 2] << 8 | buf.data[i + 3];
			if (marker == 0xDA)
			{
				scanStart = i + 2 + len;
				break;
			}
			if (marker == 0xC4)
			{
				USIZE p = i + 4;
				USIZE end = i + 2 + len;
				while (p + 17 <= end)
				{
					UINT8 tcTh = buf.data[p++];
					MiniHuffTable *table = (tcTh & 0x10) ? ((tcTh & 1) ? &acChroma : &acLuma)
					                                     : ((tcTh & 1) ? &dcChroma : &dcLuma);
					UINT8 bits[16];
					UINT32 numVals = 0;
					for (INT32 b = 0; b < 16; b++)
					{
						bits[b] = buf.data[p + b];
						numVals += bits[b];
					}
					p += 16;
					BuildMiniHuff(bits, buf.data + p, *table);
					p += numVals;
				}
			}
			i += 2 + len;
		}
		if (scanStart == 0 || scanStart + 1 >= buf.size)
		{
			LOG_ERROR("SOS marker not found in %u bytes", (UINT32)buf.size);
			return false;
		}

		// One 16x16 MCU in T.81 A.2.3 order Y1 Y2 Y3 Y4 Cb Cr — the pins ride
		// on blocks 0, 4, and 5
		ScanReader reader;
		reader.data = buf.data;
		reader.size = buf.size;
		reader.byte = scanStart;
		reader.bit = 0;
		INT32 dc[6];
		for (INT32 b = 0; b < 6; b++)
			dc[b] = (b < 4) ? DecodeBlockDC(reader, dcLuma, acLuma)
			                : DecodeBlockDC(reader, dcChroma, acChroma);

		const INT32 expected[3] = {-52, -43, 128}; // Y1, Cb, Cr
		const INT32 got[3] = {dc[0], dc[4], dc[5]};
		for (INT32 b = 0; b < 3; b++)
		{
			if (got[b] != expected[b])
			{
				LOG_ERROR("Pure-red DC pin %d: expected %d, got %d (Y1/Cb/Cr)", b, expected[b], got[b]);
				return false;
			}
		}
		return true;
	}
};
