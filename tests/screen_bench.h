#pragma once

#include "lib/runtime.h"
#include "platform/screen/screen.h"
#include "core/containers/buffer.h"
#include "tests.h"

// =============================================================================
// Screen Bench - diff / JPEG-encode / pipeline micro-benchmarks over synthetic
// 1080p frames (no display needed). Opt-in (PIR_BENCH=1): logs measurements,
// asserts only stage sanity (dirty rects found, encode succeeds).
// For real-capture numbers run under: xvfb-run -a -s "-screen 0 1920x1080x24"
// =============================================================================

class ScreenBench
{
private:
	static constexpr UINT32 Width = 1920;
	static constexpr UINT32 Height = 1080;
	static constexpr USIZE PixelCount = (USIZE)Width * Height;

	// Growable output sink mirroring the beacon JpegBuffer growth contract
	struct OutBuffer
	{
		PUINT8 data;
		USIZE capacity;
		USIZE offset;
		BOOL failed;

		OutBuffer() : data(nullptr), capacity(0), offset(0), failed(false) {}
		~OutBuffer() { delete[] data; }

		VOID Reset() { offset = 0; failed = false; }
	};

	static VOID BenchJpegCallback(PVOID context, PVOID data, INT32 size)
	{
		OutBuffer *out = (OutBuffer *)context;
		if (out->failed)
			return;
		if (out->data == nullptr)
		{
			out->data = new UINT8[1024 * 1024];
			if (!out->data)
			{
				out->failed = true;
				return;
			}
			out->capacity = 1024 * 1024;
		}
		if (out->offset + (USIZE)size > out->capacity)
		{
			USIZE newCapacity = out->capacity * 2;
			PUINT8 grown = new UINT8[newCapacity];
			if (!grown)
			{
				out->failed = true;
				return;
			}
			Memory::Copy(grown, out->data, out->offset);
			delete[] out->data;
			out->data = grown;
			out->capacity = newCapacity;
		}
		Memory::Copy(out->data + out->offset, data, (USIZE)size);
		out->offset += (USIZE)size;
	}

	static UINT64 Median(UINT64 *samples, UINT32 count)
	{
		for (UINT32 i = 1; i < count; i++)
		{
			UINT64 key = samples[i];
			INT32 j = (INT32)i - 1;
			while (j >= 0 && samples[j] > key)
			{
				samples[j + 1] = samples[j];
				j--;
			}
			samples[j + 1] = key;
		}
		return samples[count / 2];
	}

	// 8x8 blocks of low-variance color: DCT-friendly, like real screen content
	static VOID FillDesktopLike(Span<RGB> frame, Prng &prng)
	{
		for (UINT32 by = 0; by < Height; by += 8)
		{
			for (UINT32 bx = 0; bx < Width; bx += 8)
			{
				RGB base;
				base.Red = (UINT8)(prng.Get() & 0x7F);
				base.Green = (UINT8)(prng.Get() & 0x7F);
				base.Blue = (UINT8)(prng.Get() & 0x7F);
				UINT8 jitterMask = (UINT8)(prng.Get() & 0x03);
				for (UINT32 y = by; y < by + 8 && y < Height; y++)
				{
					for (UINT32 x = bx; x < bx + 8 && x < Width; x++)
					{
						RGB &p = frame[(USIZE)y * Width + x];
						p.Red = (UINT8)(base.Red + (prng.Get() & jitterMask));
						p.Green = (UINT8)(base.Green + (prng.Get() & jitterMask));
						p.Blue = (UINT8)(base.Blue + (prng.Get() & jitterMask));
					}
				}
			}
		}
	}

	// Overwrite a region with high-contrast content (well past the SAD threshold)
	static VOID MutateRegion(Span<RGB> frame, UINT32 x, UINT32 y, UINT32 w, UINT32 h, Prng &prng)
	{
		for (UINT32 row = y; row < y + h && row < Height; row++)
		{
			for (UINT32 col = x; col < x + w && col < Width; col++)
			{
				RGB &p = frame[(USIZE)row * Width + col];
				p.Red = (UINT8)(prng.Get() & 0xFF);
				p.Green = (UINT8)(prng.Get() & 0xFF);
				p.Blue = (UINT8)(prng.Get() & 0xFF);
			}
		}
	}

	// Frame B = copy of A with typical desktop mutations (cursor, tooltip, window)
	static VOID BuildDirtyPair(RGB *current, RGB *previous)
	{
		Prng prng(0xAB1E5EED);
		FillDesktopLike(Span<RGB>(current, PixelCount), prng);
		Memory::Copy(previous, current, PixelCount * sizeof(RGB));

		Prng mutator(0xD11E7EA7);
		for (UINT32 i = 0; i < 5; i++)
		{
			UINT32 x = 128 + i * 301;
			UINT32 y = 96 + i * 173;
			MutateRegion(Span<RGB>(previous, PixelCount), x, y, 64, 64, mutator);
		}
		MutateRegion(Span<RGB>(previous, PixelCount), 100, 200, 512, 256, mutator);
	}

	static BOOL TestDiffStages()
	{
		RGB *current = new RGB[PixelCount];
		RGB *previous = new RGB[PixelCount];
		UINT8 *bidiff = new UINT8[PixelCount];
		if (!current || !previous || !bidiff)
		{
			delete[] current;
			delete[] previous;
			delete[] bidiff;
			return false;
		}
		BuildDirtyPair(current, previous);

		UINT64 diffNs[5], rectsNs[5];
		UINT32 rectCount = 0;
		for (UINT32 r = 0; r < 5; r++)
		{
			UINT64 t0 = DateTime::GetMonotonicNanoseconds();
			ImageProcessor::CalculateBiDifference(Span<const RGB>(current, PixelCount),
			                                      Span<const RGB>(previous, PixelCount),
			                                      Width, Height, Span<UINT8>(bidiff, PixelCount), 24);
			UINT64 t1 = DateTime::GetMonotonicNanoseconds();
			diffNs[r] = t1 - t0;

			t0 = DateTime::GetMonotonicNanoseconds();
			auto dirty = ImageProcessor::FindDirtyRects(Span<const UINT8>(bidiff, PixelCount), Width, Height, 64);
			t1 = DateTime::GetMonotonicNanoseconds();
			rectsNs[r] = t1 - t0;
			if (!dirty)
			{
				LOG_ERROR("  FindDirtyRects failed: %e", dirty.Error());
				delete[] current;
				delete[] previous;
				delete[] bidiff;
				return false;
			}
			rectCount = dirty.Value().Count;
			dirty.Value().Free();
		}

		LOG_INFO("  CalculateBiDifference 1080p (SAD thr 24): %.2f ms (%.1f MPix/s)",
		         (DOUBLE)Median(diffNs, 5) / 1000000.0, (DOUBLE)PixelCount / (DOUBLE)Median(diffNs, 5) * 1000.0);
		LOG_INFO("  FindDirtyRects 1080p (64px tiles):       %.2f ms",
		         (DOUBLE)Median(rectsNs, 5) / 1000000.0);
		LOG_INFO("  dirty rects detected: %u", rectCount);

		delete[] current;
		delete[] previous;
		delete[] bidiff;
		return rectCount > 0;
	}

	static BOOL TestJpegEncode()
	{
		RGB *frame = new RGB[PixelCount];
		if (!frame)
			return false;
		Prng prng(0xAB1E5EED);
		FillDesktopLike(Span<RGB>(frame, PixelCount), prng);

		OutBuffer out;
		BOOL ok = true;

		// Full-frame sweep across qualities (median of 3 — each encode is slow)
		const UINT32 qualities[3] = {25, 75, 95};
		for (UINT32 qi = 0; qi < 3 && ok; qi++)
		{
			UINT32 q = qualities[qi];
			UINT64 ns[3];
			for (UINT32 r = 0; r < 3; r++)
			{
				out.Reset();
				UINT64 t0 = DateTime::GetMonotonicNanoseconds();
				auto result = JpegEncoder::Encode(BenchJpegCallback, &out, (INT32)q,
				                                  (INT32)Width, (INT32)Height, 3,
				                                  Span<const UINT8>((UINT8 *)frame, PixelCount * sizeof(RGB)));
				UINT64 t1 = DateTime::GetMonotonicNanoseconds();
				if (result.IsErr() || out.failed)
				{
					LOG_ERROR("  full-frame encode failed at q%u", q);
					ok = false;
					break;
				}
				ns[r] = t1 - t0;
			}
			if (ok)
				LOG_INFO("  JPEG 1920x1080 q%u: %.1f ms, %u B output", q,
				         (DOUBLE)Median(ns, 3) / 1000000.0, (UINT32)out.offset);
		}

		// Typical dirty-rect sizes at the default quality (median of 5)
		if (ok)
		{
			struct
			{
				UINT32 x, y, w, h;
			} rects[2] = {{100, 200, 512, 256}, {320, 448, 64, 64}};
			RGB *packed = new RGB[512 * 256];
			if (!packed)
				ok = false;
			for (UINT32 i = 0; i < 2 && ok; i++)
			{
				for (UINT32 row = 0; row < rects[i].h; row++)
					Memory::Copy(packed + (USIZE)row * rects[i].w,
					             frame + (USIZE)(rects[i].y + row) * Width + rects[i].x,
					             (USIZE)rects[i].w * sizeof(RGB));

				UINT64 ns[5];
				for (UINT32 r = 0; r < 5; r++)
				{
					out.Reset();
					UINT64 t0 = DateTime::GetMonotonicNanoseconds();
					auto result = JpegEncoder::Encode(BenchJpegCallback, &out, 75,
					                                  (INT32)rects[i].w, (INT32)rects[i].h, 3,
					                                  Span<const UINT8>((UINT8 *)packed, (USIZE)rects[i].w * rects[i].h * sizeof(RGB)));
					UINT64 t1 = DateTime::GetMonotonicNanoseconds();
					if (result.IsErr() || out.failed)
					{
						LOG_ERROR("  rect encode failed (%ux%u)", rects[i].w, rects[i].h);
						ok = false;
						break;
					}
					ns[r] = t1 - t0;
				}
				if (ok)
					LOG_INFO("  JPEG %ux%u q75: %.2f ms, %u B output", rects[i].w, rects[i].h,
					         (DOUBLE)Median(ns, 5) / 1000000.0, (UINT32)out.offset);
			}
			delete[] packed;
		}

		delete[] frame;
		return ok;
	}

	static BOOL TestPipeline()
	{
		RGB *current = new RGB[PixelCount];
		RGB *previous = new RGB[PixelCount];
		UINT8 *bidiff = new UINT8[PixelCount];
		RGB *rectBuffer = new RGB[PixelCount];
		if (!current || !previous || !bidiff || !rectBuffer)
		{
			delete[] current;
			delete[] previous;
			delete[] bidiff;
			delete[] rectBuffer;
			return false;
		}
		BuildDirtyPair(current, previous);

		OutBuffer jpeg;
		Buffer<CHAR> packet;
		UINT64 ns[5];
		UINT32 totalRects = 0;
		USIZE packetBytes = 0;
		for (UINT32 r = 0; r < 5; r++)
		{
			packet.Reset();
			jpeg.Reset();
			UINT64 t0 = DateTime::GetMonotonicNanoseconds();

			ImageProcessor::CalculateBiDifference(Span<const RGB>(current, PixelCount),
			                                      Span<const RGB>(previous, PixelCount),
			                                      Width, Height, Span<UINT8>(bidiff, PixelCount), 24);
			auto dirty = ImageProcessor::FindDirtyRects(Span<const UINT8>(bidiff, PixelCount), Width, Height, 64);
			if (!dirty)
			{
				LOG_ERROR("  FindDirtyRects failed: %e", dirty.Error());
				break;
			}

			if (!packet.Init(sizeof(UINT32) * 2 + PixelCount * sizeof(RGB) / 2))
				break;
			UINT32 packetOffset = sizeof(UINT32) * 2;

			for (UINT32 i = 0; i < dirty.Value().Count; i++)
			{
				const DirtyRect &dr = dirty.Value().Rects[i];
				for (UINT32 row = 0; row < dr.Height; row++)
					Memory::Copy(rectBuffer + (USIZE)row * dr.Width,
					             current + (USIZE)(dr.Y + row) * Width + dr.X,
					             (USIZE)dr.Width * sizeof(RGB));

				jpeg.Reset();
				auto encode = JpegEncoder::Encode(BenchJpegCallback, &jpeg, 75,
				                                  (INT32)dr.Width, (INT32)dr.Height, 3,
				                                  Span<const UINT8>((UINT8 *)rectBuffer, (USIZE)dr.Width * dr.Height * sizeof(RGB)));
				if (encode.IsErr() || jpeg.failed)
				{
					dirty.Value().Free();
					break;
				}

				Memory::Copy(packet.Data + packetOffset, &dr.X, sizeof(UINT32));
				Memory::Copy(packet.Data + packetOffset + 4, &dr.Y, sizeof(UINT32));
				Memory::Copy(packet.Data + packetOffset + 8, &jpeg.offset, sizeof(UINT32));
				Memory::Copy(packet.Data + packetOffset + 12, jpeg.data, jpeg.offset);
				packetOffset += 12 + jpeg.offset;
			}

			totalRects = dirty.Value().Count;
			dirty.Value().Free();
			UINT64 t1 = DateTime::GetMonotonicNanoseconds();
			ns[r] = t1 - t0;
			packetBytes = packetOffset;

			// Release the packet allocation so the next rep starts fresh
			delete[] packet.Release();
			jpeg.Reset();
		}

		DOUBLE ms = (DOUBLE)Median(ns, 5) / 1000000.0;
		LOG_INFO("  diff->rects->encode->packet pipeline 1080p: %.2f ms (max ~%.1f FPS agent-side)",
		         ms, 1000.0 / ms);
		LOG_INFO("  %u rects, %u B reply packet", totalRects, (UINT32)packetBytes);

		delete[] current;
		delete[] previous;
		delete[] bidiff;
		delete[] rectBuffer;
		return totalRects > 0 && packetBytes > 0;
	}

	static BOOL TestCapture()
	{
		auto devices = Screen::GetDevices();
		if (!devices)
		{
			LOG_INFO("  SKIP: no display available (headless). Run under xvfb-run for capture numbers");
			return true;
		}

		const ScreenDevice &device = devices.Value().Devices[0];
		USIZE pixels = (USIZE)device.Width * device.Height;
		RGB *buffer = new RGB[pixels];
		if (!buffer)
		{
			devices.Value().Free();
			return false;
		}

		BOOL ok = true;
		BOOL captured = false;
		UINT32 captureCount = 0;
		UINT64 ns[10];
		for (UINT32 r = 0; r < 10; r++)
		{
			UINT64 t0 = DateTime::GetMonotonicNanoseconds();
			auto capture = Screen::Capture(device, Span<RGB>(buffer, pixels));
			UINT64 t1 = DateTime::GetMonotonicNanoseconds();
			if (!capture)
			{
				// Headless: enumeration succeeds but no real framebuffer — a bench
				// condition, not a correctness failure (the gate suite covers that)
				LOG_INFO("  SKIP: capture unavailable on this display (error %e)", capture.Error());
				break;
			}
			ns[r] = t1 - t0;
			captureCount++;
			captured = true;
		}

		if (captured)
			LOG_INFO("  Screen::Capture %ux%u: %.2f ms/frame (median of %u)",
			         device.Width, device.Height, (DOUBLE)Median(ns, captureCount) / 1000000.0, captureCount);

		delete[] buffer;
		devices.Value().Free();
		return ok;
	}

public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Screen Bench (synthetic 1080p)...");
		LOG_INFO("  Test: diff stages");
		RunTest(allPassed, &TestDiffStages, "diff stages sanity");
		LOG_INFO("  Test: JPEG encode");
		RunTest(allPassed, &TestJpegEncode, "jpeg encode sanity");
		LOG_INFO("  Test: end-to-end pipeline");
		RunTest(allPassed, &TestPipeline, "pipeline sanity");
		LOG_INFO("  Test: real capture");
		RunTest(allPassed, &TestCapture, "capture sanity");

		if (allPassed)
			LOG_INFO("Screen bench complete!");
		else
			LOG_ERROR("Some screen bench tests failed!");

		return allPassed;
	}
};
