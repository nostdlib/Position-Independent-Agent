#pragma once

#include "lib/runtime.h"
#include "tests.h"

class ImageTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Image Processing Tests...");

		// BiDifference tests
		RunTest(allPassed, &TestBiDiff_IdenticalImages, "BiDiff identical images all zero");
		RunTest(allPassed, &TestBiDiff_CompletelyDifferent, "BiDiff completely different images all one");
		RunTest(allPassed, &TestBiDiff_PartialDifference, "BiDiff partial difference");
		RunTest(allPassed, &TestBiDiff_SingleChannelDifference, "BiDiff single channel difference detected");
		RunTest(allPassed, &TestBiDiff_ThresholdBelowIgnored, "BiDiff threshold filters small differences");
		RunTest(allPassed, &TestBiDiff_ThresholdAboveDetected, "BiDiff threshold passes large differences");

		// FindDirtyRects tests
		RunTest(allPassed, &TestDirtyRects_AllClean, "FindDirtyRects all-zero returns no rects");
		RunTest(allPassed, &TestDirtyRects_SingleTile, "FindDirtyRects single dirty tile");
		RunTest(allPassed, &TestDirtyRects_HorizontalMerge, "FindDirtyRects adjacent tiles merge horizontally");
		RunTest(allPassed, &TestDirtyRects_VerticalMerge, "FindDirtyRects adjacent tiles merge vertically");
		RunTest(allPassed, &TestDirtyRects_TwoSeparateRegions, "FindDirtyRects two separate dirty regions");
		RunTest(allPassed, &TestDirtyRects_SmallRegionFiltered, "FindDirtyRects region < 32x32 filtered out");

		// Right-edge tests: width not a multiple of tile size (1366-wide frames)
		RunTest(allPassed, &TestDirtyRects_RightEdgeTileOnly, "FindDirtyRects right-edge-only change on 1366-wide frame");
		RunTest(allPassed, &TestDirtyRects_RightEdgeMultiRow, "FindDirtyRects right-edge change spanning rows (1366x768)");
		RunTest(allPassed, &TestDirtyRects_RightEdgePartialRun, "FindDirtyRects run ending at right edge reaches image edge");
		RunTest(allPassed, &TestDirtyRects_MultipleOf64Width, "FindDirtyRects multiple-of-64 width unchanged");
		RunTest(allPassed, &TestDirtyRects_RightEdgePlusInterior, "FindDirtyRects right-edge plus interior changes");

		// Fused FindDirtyRects equivalence vs the two-step pipeline
		RunTest(allPassed, &TestFused_IdenticalFrames, "Fused diff identical frames matches two-step");
		RunTest(allPassed, &TestFused_SingleDirtyTile, "Fused diff single and merged tiles match two-step");
		RunTest(allPassed, &TestFused_BelowThresholdNoise, "Fused diff below-threshold noise matches two-step");
		RunTest(allPassed, &TestFused_EdgeTileDirty, "Fused diff edge tile matches two-step");
		RunTest(allPassed, &TestFused_ZeroThreshold, "Fused diff zero threshold matches two-step");
		RunTest(allPassed, &TestFused_OddImageSize, "Fused diff non-multiple-of-tile size matches two-step");

		// Fused path against hand-computed rects at the right edge (both paths
		// share MergeDirtyTiles, so equivalence alone cannot catch edge bugs)
		RunTest(allPassed, &TestFused_RightEdgeTileOnly, "Fused diff right-edge-only rect on 1366-wide frame");
		RunTest(allPassed, &TestFused_RightEdgePlusInterior, "Fused diff right-edge plus interior rects");
		RunTest(allPassed, &TestFused_DriftAccumulates, "Fused diff sub-threshold drift reverts and accumulates");

		// Threshold boundary: SAD exactly at, one under, one over the threshold
		RunTest(allPassed, &TestFused_ThresholdBoundary, "Fused diff SAD at/under/over threshold 24");
		RunTest(allPassed, &TestFused_SinglePixelChange, "Fused diff single changed pixel marks its tile");

		// Edge-clamp shapes beyond the right edge: odd widths, bottom edge, corner
		RunTest(allPassed, &TestDirtyRects_OddWidthRightEdge, "FindDirtyRects right-edge tile at odd widths");
		RunTest(allPassed, &TestDirtyRects_BottomEdgeRow, "FindDirtyRects bottom-edge row clamps height");
		RunTest(allPassed, &TestDirtyRects_CornerTileBothEdges, "FindDirtyRects corner tile clamps width and height");
		RunTest(allPassed, &TestDirtyRects_Tile32RightEdge, "FindDirtyRects tile-32 right edge passes span filter");

		// Merge shapes and whole-frame coverage
		RunTest(allPassed, &TestDirtyRects_StaircaseMerge, "FindDirtyRects staircase tiles stay separate rects");
		RunTest(allPassed, &TestDirtyRects_FullFrameDirty, "FindDirtyRects full-frame dirty yields one clamped rect");
		RunTest(allPassed, &TestFused_StaircaseRects, "Fused diff staircase rects match hand-computed");
		RunTest(allPassed, &TestFused_CornerTileRects, "Fused diff corner tile rect clamps both edges");
		RunTest(allPassed, &TestFused_FullFrameChange, "Fused diff full-frame change yields one clamped rect");

		// Statelessness across calls and multi-frame drift accumulation
		RunTest(allPassed, &TestFused_AlternatingRegions, "Fused diff alternating regions reflect only the current frame");
		RunTest(allPassed, &TestFused_DriftAccumulatesMultiFrame, "Fused diff multi-frame drift stays clean until crossing");

		if (allPassed)
			LOG_INFO("All Image Processing tests passed!");
		else
			LOG_ERROR("Some Image Processing tests failed!");

		return allPassed;
	}

private:
	// ---- BiDifference tests ----

	static BOOL TestBiDiff_IdenticalImages()
	{
		RGB img[4];
		img[0] = {10, 20, 30};
		img[1] = {40, 50, 60};
		img[2] = {70, 80, 90};
		img[3] = {100, 110, 120};

		UINT8 diff[4];
		ImageProcessor::CalculateBiDifference(img, img, 2, 2, diff);

		for (INT32 i = 0; i < 4; i++)
		{
			if (diff[i] != 0)
			{
				LOG_ERROR("Expected diff[%d] = 0, got %d", i, diff[i]);
				return false;
			}
		}
		return true;
	}

	static BOOL TestBiDiff_CompletelyDifferent()
	{
		RGB img1[4];
		img1[0] = {0, 0, 0};
		img1[1] = {0, 0, 0};
		img1[2] = {0, 0, 0};
		img1[3] = {0, 0, 0};

		RGB img2[4];
		img2[0] = {255, 255, 255};
		img2[1] = {255, 255, 255};
		img2[2] = {255, 255, 255};
		img2[3] = {255, 255, 255};

		UINT8 diff[4];
		ImageProcessor::CalculateBiDifference(img1, img2, 2, 2, diff);

		for (INT32 i = 0; i < 4; i++)
		{
			if (diff[i] != 1)
			{
				LOG_ERROR("Expected diff[%d] = 1, got %d", i, diff[i]);
				return false;
			}
		}
		return true;
	}

	static BOOL TestBiDiff_PartialDifference()
	{
		RGB img1[4];
		img1[0] = {10, 20, 30};
		img1[1] = {40, 50, 60};
		img1[2] = {70, 80, 90};
		img1[3] = {100, 110, 120};

		RGB img2[4];
		img2[0] = {10, 20, 30}; // same
		img2[1] = {40, 50, 61}; // different (Blue)
		img2[2] = {70, 80, 90}; // same
		img2[3] = {99, 110, 120}; // different (Red)

		UINT8 diff[4];
		ImageProcessor::CalculateBiDifference(img1, img2, 2, 2, diff);

		if (diff[0] != 0 || diff[1] != 1 || diff[2] != 0 || diff[3] != 1)
		{
			LOG_ERROR("Partial diff mismatch: %d %d %d %d", diff[0], diff[1], diff[2], diff[3]);
			return false;
		}
		return true;
	}

	static BOOL TestBiDiff_SingleChannelDifference()
	{
		RGB img1[1];
		img1[0] = {100, 200, 50};

		RGB img2[1];
		img2[0] = {100, 201, 50}; // only Green differs

		UINT8 diff[1];
		ImageProcessor::CalculateBiDifference(img1, img2, 1, 1, diff);

		if (diff[0] != 1)
		{
			LOG_ERROR("Single channel diff not detected");
			return false;
		}
		return true;
	}

	static BOOL TestBiDiff_ThresholdBelowIgnored()
	{
		RGB img1[1];
		img1[0] = {100, 100, 100};

		RGB img2[1];
		img2[0] = {105, 103, 102}; // SAD = 5+3+2 = 10

		UINT8 diff[1];
		ImageProcessor::CalculateBiDifference(img1, img2, 1, 1, diff, 10);

		if (diff[0] != 0)
		{
			LOG_ERROR("SAD <= threshold should yield 0, got %d", diff[0]);
			return false;
		}
		return true;
	}

	static BOOL TestBiDiff_ThresholdAboveDetected()
	{
		RGB img1[1];
		img1[0] = {100, 100, 100};

		RGB img2[1];
		img2[0] = {105, 103, 102}; // SAD = 10

		UINT8 diff[1];
		ImageProcessor::CalculateBiDifference(img1, img2, 1, 1, diff, 9);

		if (diff[0] != 1)
		{
			LOG_ERROR("SAD > threshold should yield 1, got %d", diff[0]);
			return false;
		}
		return true;
	}

	// ---- FindDirtyRects tests ----

	static BOOL TestDirtyRects_AllClean()
	{
		// 64x64 all-zero biDiff, tile size 32 → no dirty rects
		constexpr UINT32 sz = 64;
		UINT8 biDiff[sz * sz];
		Memory::Zero(biDiff, sizeof(biDiff));

		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, sz * sz), sz, sz, 32);
		if (!r)
		{
			LOG_ERROR("FindDirtyRects failed: %e", r.Error());
			return false;
		}

		auto result = r.Value();
		if (result.Count != 0)
		{
			LOG_ERROR("Expected 0 rects, got %u", result.Count);
			result.Free();
			return false;
		}

		result.Free();
		return true;
	}

	static BOOL TestDirtyRects_SingleTile()
	{
		// 64x64 image, tile size 32 → 2x2 tiles
		// Mark one pixel dirty in tile (0,0)
		constexpr UINT32 sz = 64;
		UINT8 biDiff[sz * sz];
		Memory::Zero(biDiff, sizeof(biDiff));
		biDiff[0] = 1; // top-left tile

		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, sz * sz), sz, sz, 32);
		if (!r)
		{
			LOG_ERROR("FindDirtyRects failed: %e", r.Error());
			return false;
		}

		auto result = r.Value();
		if (result.Count != 1)
		{
			LOG_ERROR("Expected 1 rect, got %u", result.Count);
			result.Free();
			return false;
		}

		// Should be at tile (0,0): X=0, Y=0, 32x32
		if (result.Rects[0].X != 0 || result.Rects[0].Y != 0 ||
			result.Rects[0].Width != 32 || result.Rects[0].Height != 32)
		{
			LOG_ERROR("Rect mismatch: X=%u Y=%u W=%u H=%u",
				result.Rects[0].X, result.Rects[0].Y,
				result.Rects[0].Width, result.Rects[0].Height);
			result.Free();
			return false;
		}

		result.Free();
		return true;
	}

	static BOOL TestDirtyRects_HorizontalMerge()
	{
		// 128x64 image, tile size 32 → 4x2 tiles
		// Mark pixels dirty in tiles (0,0) and (1,0) → should merge into one 64x32 rect
		constexpr UINT32 w = 128;
		constexpr UINT32 h = 64;
		UINT8 biDiff[w * h];
		Memory::Zero(biDiff, sizeof(biDiff));
		biDiff[0] = 1;   // tile (0,0)
		biDiff[32] = 1;  // tile (1,0) — pixel at x=32, y=0

		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, w * h), w, h, 32);
		if (!r)
		{
			LOG_ERROR("FindDirtyRects failed: %e", r.Error());
			return false;
		}

		auto result = r.Value();
		if (result.Count != 1)
		{
			LOG_ERROR("Expected 1 merged rect, got %u", result.Count);
			result.Free();
			return false;
		}

		if (result.Rects[0].Width != 64 || result.Rects[0].Height != 32)
		{
			LOG_ERROR("Merge mismatch: W=%u H=%u (expected 64x32)",
				result.Rects[0].Width, result.Rects[0].Height);
			result.Free();
			return false;
		}

		result.Free();
		return true;
	}

	static BOOL TestDirtyRects_VerticalMerge()
	{
		// 64x128 image, tile size 32 → 2x4 tiles
		// Mark pixels dirty in tiles (0,0) and (0,1) → should merge into one 32x64 rect
		constexpr UINT32 w = 64;
		constexpr UINT32 h = 128;
		UINT8 biDiff[w * h];
		Memory::Zero(biDiff, sizeof(biDiff));
		biDiff[0] = 1;          // tile (0,0)
		biDiff[32 * w] = 1;     // tile (0,1) — pixel at x=0, y=32

		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, w * h), w, h, 32);
		if (!r)
		{
			LOG_ERROR("FindDirtyRects failed: %e", r.Error());
			return false;
		}

		auto result = r.Value();
		if (result.Count != 1)
		{
			LOG_ERROR("Expected 1 merged rect, got %u", result.Count);
			result.Free();
			return false;
		}

		if (result.Rects[0].Width != 32 || result.Rects[0].Height != 64)
		{
			LOG_ERROR("Merge mismatch: W=%u H=%u (expected 32x64)",
				result.Rects[0].Width, result.Rects[0].Height);
			result.Free();
			return false;
		}

		result.Free();
		return true;
	}

	static BOOL TestDirtyRects_TwoSeparateRegions()
	{
		// 128x64 image, tile size 32 → 4x2 tiles
		// Mark tile (0,0) and tile (3,0) dirty — should produce 2 rects
		constexpr UINT32 w = 128;
		constexpr UINT32 h = 64;
		UINT8 biDiff[w * h];
		Memory::Zero(biDiff, sizeof(biDiff));
		biDiff[0] = 1;    // tile (0,0)
		biDiff[96] = 1;   // tile (3,0) — pixel at x=96, y=0

		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, w * h), w, h, 32);
		if (!r)
		{
			LOG_ERROR("FindDirtyRects failed: %e", r.Error());
			return false;
		}

		auto result = r.Value();
		if (result.Count != 2)
		{
			LOG_ERROR("Expected 2 rects, got %u", result.Count);
			result.Free();
			return false;
		}

		result.Free();
		return true;
	}

	static BOOL TestDirtyRects_SmallRegionFiltered()
	{
		// 32x32 image, tile size 16 → 2x2 tiles
		// Mark only tile (0,0) dirty → 16x16 rect → filtered (< 32x32)
		constexpr UINT32 sz = 32;
		UINT8 biDiff[sz * sz];
		Memory::Zero(biDiff, sizeof(biDiff));
		biDiff[0] = 1; // tile (0,0) only

		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, sz * sz), sz, sz, 16);
		if (!r)
		{
			LOG_ERROR("FindDirtyRects failed: %e", r.Error());
			return false;
		}

		auto result = r.Value();
		if (result.Count != 0)
		{
			LOG_ERROR("Expected 0 rects (filtered), got %u", result.Count);
			result.Free();
			return false;
		}

		result.Free();
		return true;
	}

	// ---- Right-edge tests (width not a multiple of tile size) ----
	// A 1366-wide frame has a 22-px right-edge tile column (tilesX = 22);
	// rects there must extend exactly to the image edge, never be dropped.

	// Compare a DirtyRectResult against hand-computed expected rects
	static BOOL RectsMatch(const DirtyRectResult &result, const DirtyRect *expected,
	                       UINT32 expectedCount, PCCHAR label)
	{
		if (result.Count != expectedCount)
		{
			LOG_ERROR("%s: expected %u rects, got %u", label, expectedCount, result.Count);
			return false;
		}
		for (UINT32 i = 0; i < expectedCount; i++)
		{
			const DirtyRect &got = result.Rects[i];
			if (got.X != expected[i].X || got.Y != expected[i].Y ||
				got.Width != expected[i].Width || got.Height != expected[i].Height)
			{
				LOG_ERROR("%s: rect %u got X=%u Y=%u W=%u H=%u, want X=%u Y=%u W=%u H=%u",
					label, i, got.X, got.Y, got.Width, got.Height,
					expected[i].X, expected[i].Y, expected[i].Width, expected[i].Height);
				return false;
			}
		}
		return true;
	}

	// Mark one pixel per dirty tile ((tx,ty) pairs) and check the two-step overload
	static BOOL RunTwoStepTileCase(UINT32 width, UINT32 height, UINT32 tileSize,
	                               const UINT32 *tiles, UINT32 tileCount,
	                               const DirtyRect *expected, UINT32 expectedCount,
	                               PCCHAR label)
	{
		UINT8 *biDiff = new UINT8[(USIZE)width * height];
		if (!biDiff)
			return false;
		Memory::Zero(biDiff, (USIZE)width * height);
		for (UINT32 i = 0; i < tileCount; i++)
			biDiff[(USIZE)(tiles[i * 2 + 1] * tileSize) * width + tiles[i * 2] * tileSize] = 1;

		BOOL pass = false;
		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, (USIZE)width * height), width, height, tileSize);
		if (!r)
			LOG_ERROR("%s: FindDirtyRects failed: %e", label, r.Error());
		else
		{
			pass = RectsMatch(r.Value(), expected, expectedCount, label);
			r.Value().Free();
		}
		delete[] biDiff;
		return pass;
	}

	// 1366x128, tile 64 → 22x2 grid; tile (21,0) covers x 1344..1366 (22 px)
	static BOOL TestDirtyRects_RightEdgeTileOnly()
	{
		const UINT32 tiles[] = {21, 0};
		const DirtyRect expected[] = {{1344, 0, 22, 64}};
		return RunTwoStepTileCase(1366, 128, 64, tiles, 1, expected, 1, "RightEdgeTileOnly");
	}

	// 1366x768 real resolution; tiles (21,5)+(21,6) merge vertically
	static BOOL TestDirtyRects_RightEdgeMultiRow()
	{
		const UINT32 tiles[] = {21, 5, 21, 6};
		const DirtyRect expected[] = {{1344, 320, 22, 128}};
		return RunTwoStepTileCase(1366, 768, 64, tiles, 2, expected, 1, "RightEdgeMultiRow");
	}

	// Run of tiles 19..21 must reach x=1366 exactly (width 150, not 148)
	static BOOL TestDirtyRects_RightEdgePartialRun()
	{
		const UINT32 tiles[] = {19, 0, 20, 0, 21, 0};
		const DirtyRect expected[] = {{1216, 0, 150, 64}};
		return RunTwoStepTileCase(1366, 128, 64, tiles, 3, expected, 1, "RightEdgePartialRun");
	}

	// Multiple-of-64 width: no clamping — behavior must not change
	static BOOL TestDirtyRects_MultipleOf64Width()
	{
		const UINT32 tiles[] = {19, 0};
		const DirtyRect expected[] = {{1216, 0, 64, 64}};
		return RunTwoStepTileCase(1280, 128, 64, tiles, 1, expected, 1, "MultipleOf64Width");
	}

	// Right-edge tile (21,0) and interior tile (5,1): two rects, scan order
	static BOOL TestDirtyRects_RightEdgePlusInterior()
	{
		const UINT32 tiles[] = {21, 0, 5, 1};
		const DirtyRect expected[] = {{1344, 0, 22, 64}, {320, 64, 64, 64}};
		return RunTwoStepTileCase(1366, 128, 64, tiles, 2, expected, 2, "RightEdgePlusInterior");
	}

	// ---- Fused FindDirtyRects equivalence tests ----
	// The fused overload must produce the exact same rectangles as
	// CalculateBiDifference + FindDirtyRects run in sequence.

	// Run both pipelines on the same frame pair and compare rect-by-rect
	static BOOL CompareFusedVsTwoStep(RGB *current, const RGB *previous,
	                                  UINT32 width, UINT32 height, UINT32 threshold)
	{
		UINT8 *biDiff = new UINT8[(USIZE)width * height];
		if (!biDiff)
			return false;

		ImageProcessor::CalculateBiDifference(
			Span<const RGB>(current, (USIZE)width * height),
			Span<const RGB>(previous, (USIZE)width * height),
			width, height, Span<UINT8>(biDiff, (USIZE)width * height), threshold);
		auto twoStep = ImageProcessor::FindDirtyRects(
			Span<const UINT8>(biDiff, (USIZE)width * height), width, height, 64);
		auto fused = ImageProcessor::FindDirtyRects(
			Span<RGB>(current, (USIZE)width * height),
			Span<const RGB>(previous, (USIZE)width * height),
			width, height, 64, threshold);

		BOOL equal = twoStep && fused;
		if (equal)
		{
			equal = twoStep.Value().Count == fused.Value().Count;
			for (UINT32 i = 0; equal && i < twoStep.Value().Count; i++)
			{
				const DirtyRect &a = twoStep.Value().Rects[i];
				const DirtyRect &b = fused.Value().Rects[i];
				equal = a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
			}
		}

		if (twoStep)
			twoStep.Value().Free();
		if (fused)
			fused.Value().Free();
		delete[] biDiff;
		return equal;
	}

	// Frame pair factory: flat base, then per-case mutation callback
	static BOOL BuildFramePair(UINT32 width, UINT32 height, RGB **currentOut, RGB **previousOut,
	                           VOID (*mutate)(RGB *frame, UINT32 width, UINT32 height))
	{
		RGB *current = new RGB[(USIZE)width * height];
		RGB *previous = new RGB[(USIZE)width * height];
		if (!current || !previous)
		{
			delete[] current;
			delete[] previous;
			return false;
		}

		for (USIZE i = 0; i < (USIZE)width * height; i++)
		{
			current[i].Red = 40;
			current[i].Green = 60;
			current[i].Blue = 80;
		}
		Memory::Copy(previous, current, (USIZE)width * height * sizeof(RGB));
		if (mutate != nullptr)
			mutate(previous, width, height);

		*currentOut = current;
		*previousOut = previous;
		return true;
	}

	static BOOL RunFusedCase(UINT32 width, UINT32 height, UINT32 threshold,
	                         VOID (*mutate)(RGB *frame, UINT32 width, UINT32 height))
	{
		RGB *current = nullptr;
		RGB *previous = nullptr;
		if (!BuildFramePair(width, height, &current, &previous, mutate))
			return false;
		BOOL equal = CompareFusedVsTwoStep(current, previous, width, height, threshold);
		delete[] current;
		delete[] previous;
		return equal;
	}

	static VOID MutateSingleTile(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		// Bright block inside tile (1,1)
		for (UINT32 y = 70; y < 100; y++)
			for (UINT32 x = 70; x < 100; x++)
			{
				frame[(USIZE)y * width + x].Red = 240;
				frame[(USIZE)y * width + x].Green = 200;
				frame[(USIZE)y * width + x].Blue = 160;
			}
	}

	static VOID MutateTwoTilesHorizontal(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (UINT32 y = 70; y < 100; y++)
			for (UINT32 x = 70; x < 190; x++)
			{
				frame[(USIZE)y * width + x].Red = 240;
				frame[(USIZE)y * width + x].Green = 200;
				frame[(USIZE)y * width + x].Blue = 160;
			}
	}

	static VOID MutateEdgeTile(RGB *frame, UINT32 width, UINT32 height)
	{
		// Bottom-right partial tile (image is 256x192, tile grid 4x3 — this
		// stays inside the last full tile and clamps against the border)
		for (UINT32 y = height - 20; y < height; y++)
			for (UINT32 x = width - 20; x < width; x++)
			{
				frame[(USIZE)y * width + x].Red = 240;
				frame[(USIZE)y * width + x].Green = 200;
				frame[(USIZE)y * width + x].Blue = 160;
			}
	}

	static BOOL TestFused_IdenticalFrames()
	{
		return RunFusedCase(256, 192, 24, nullptr);
	}

	static BOOL TestFused_SingleDirtyTile()
	{
		// One dirty tile, and a two-tile horizontal run — both must match
		return RunFusedCase(256, 192, 24, &MutateSingleTile) &&
		       RunFusedCase(256, 192, 24, &MutateTwoTilesHorizontal);
	}

	static BOOL TestFused_BelowThresholdNoise()
	{
		// Per-channel deltas of +/-2 keep every SAD at 6 < 24: no dirty tiles.
		// One pixel with a large delta must be the only one detected.
		RGB *current = nullptr;
		RGB *previous = nullptr;
		if (!BuildFramePair(256, 192, &current, &previous, nullptr))
			return false;

		for (USIZE i = 0; i < (USIZE)256 * 192; i++)
		{
			previous[i].Red = (UINT8)(current[i].Red + (i & 1 ? 2 : -2));
			previous[i].Green = (UINT8)(current[i].Green + (i & 2 ? 2 : -2));
			previous[i].Blue = (UINT8)(current[i].Blue + (i & 4 ? 2 : -2));
		}
		BOOL noiseEqual = CompareFusedVsTwoStep(current, previous, 256, 192, 24);

		previous[5 * 256 + 5].Red = 255;
		BOOL spikeEqual = CompareFusedVsTwoStep(current, previous, 256, 192, 24);

		delete[] current;
		delete[] previous;
		return noiseEqual && spikeEqual;
	}

	static BOOL TestFused_EdgeTileDirty()
	{
		return RunFusedCase(256, 192, 24, &MutateEdgeTile);
	}

	static BOOL TestFused_ZeroThreshold()
	{
		// threshold 0: any channel change counts (SAD > 0 == XOR inequality)
		return RunFusedCase(256, 192, 0, &MutateSingleTile);
	}

	static BOOL TestFused_OddImageSize()
	{
		// 100x100 with 64px tiles: edge tiles clamp to the image border
		return RunFusedCase(100, 100, 24, &MutateSingleTile) &&
		       RunFusedCase(100, 100, 24, &MutateEdgeTile);
	}

	// Run the fused overload (production path) and check hand-computed rects
	static BOOL RunFusedRectCase(UINT32 width, UINT32 height, UINT32 threshold,
	                             VOID (*mutate)(RGB *, UINT32, UINT32),
	                             const DirtyRect *expected, UINT32 expectedCount,
	                             PCCHAR label)
	{
		RGB *current = nullptr;
		RGB *previous = nullptr;
		if (!BuildFramePair(width, height, &current, &previous, mutate))
			return false;

		BOOL pass = false;
		auto r = ImageProcessor::FindDirtyRects(
			Span<RGB>(current, (USIZE)width * height),
			Span<const RGB>(previous, (USIZE)width * height),
			width, height, 64, threshold);
		if (!r)
			LOG_ERROR("%s: fused FindDirtyRects failed: %e", label, r.Error());
		else
		{
			pass = RectsMatch(r.Value(), expected, expectedCount, label);
			r.Value().Free();
		}
		delete[] current;
		delete[] previous;
		return pass;
	}

	// Block inside the right-edge tile (21,0) of a 1366-wide frame
	static VOID MutateRightEdgeTile(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (UINT32 y = 10; y < 20; y++)
			for (UINT32 x = 1350; x < 1360; x++)
			{
				frame[(USIZE)y * width + x].Red = 240;
				frame[(USIZE)y * width + x].Green = 200;
				frame[(USIZE)y * width + x].Blue = 160;
			}
	}

	// Right-edge tile (21,0) plus interior tile (5,1)
	static VOID MutateRightEdgeAndInterior(RGB *frame, UINT32 width, UINT32 height)
	{
		MutateRightEdgeTile(frame, width, height);
		for (UINT32 y = 70; y < 80; y++)
			for (UINT32 x = 330; x < 340; x++)
			{
				frame[(USIZE)y * width + x].Red = 240;
				frame[(USIZE)y * width + x].Green = 200;
				frame[(USIZE)y * width + x].Blue = 160;
			}
	}

	static BOOL TestFused_RightEdgeTileOnly()
	{
		const DirtyRect expected[] = {{1344, 0, 22, 64}};
		return RunFusedRectCase(1366, 128, 24, &MutateRightEdgeTile, expected, 1, "FusedRightEdgeTileOnly");
	}

	static BOOL TestFused_RightEdgePlusInterior()
	{
		const DirtyRect expected[] = {{1344, 0, 22, 64}, {320, 64, 64, 64}};
		return RunFusedRectCase(1366, 128, 24, &MutateRightEdgeAndInterior, expected, 2, "FusedRightEdgePlusInterior");
	}

	// Sub-threshold drift must not be absorbed into the diff base: a clean
	// tile's small change is reverted so the base keeps the sent content and
	// the accumulated change crosses the threshold on a later capture
	static BOOL TestFused_DriftAccumulates()
	{
		const USIZE px = 64 * 64;
		RGB *base = new RGB[px];
		RGB *frame = new RGB[px];
		if (base == nullptr || frame == nullptr)
		{
			delete[] base;
			delete[] frame;
			return false;
		}
		for (USIZE i = 0; i < px; i++)
		{
			base[i] = {10, 10, 10};
			frame[i] = {18, 18, 18}; // +8 per channel = SAD 24, not over it
		}

		BOOL pass = false;
		auto r1 = ImageProcessor::FindDirtyRects(
			Span<RGB>(frame, px), Span<const RGB>(base, px), 64, 64, 64, 24);
		if (!r1)
		{
			LOG_ERROR("DriftAccumulates: fused pass failed: %e", r1.Error());
		}
		else
		{
			pass = r1.Value().Count == 0;
			r1.Value().Free();
			// The drift must be reverted: the current frame equals the base again
			for (USIZE i = 0; pass && i < px; i++)
				if (frame[i].Red != 10 || frame[i].Green != 10 || frame[i].Blue != 10)
					pass = false;
		}

		// The screen kept fading — a fresh capture is +16 per channel over the
		// ORIGINAL base and must now be detected
		if (pass)
		{
			for (USIZE i = 0; i < px; i++)
				frame[i] = {26, 26, 26};
			auto r2 = ImageProcessor::FindDirtyRects(
				Span<RGB>(frame, px), Span<const RGB>(base, px), 64, 64, 64, 24);
			if (!r2)
			{
				LOG_ERROR("DriftAccumulates: second pass failed: %e", r2.Error());
				pass = false;
			}
			else
			{
				const DirtyRect expected[] = {{0, 0, 64, 64}};
				pass = RectsMatch(r2.Value(), expected, 1, "DriftAccumulates");
				r2.Value().Free();
			}
		}

		delete[] base;
		delete[] frame;
		return pass;
	}

	// ---- Threshold boundary tests ----

	// Base frame is (40,60,80); each mutator sets an exact per-channel delta
	static VOID MutateSadExactlyThreshold(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {48, 68, 88}; // +8/+8/+8 = SAD 24
	}

	static VOID MutateSadJustUnder(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {48, 68, 87}; // +8/+8/+7 = SAD 23
	}

	static VOID MutateSadJustOver(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {48, 68, 89}; // +8/+8/+9 = SAD 25
	}

	static VOID MutateSadSingleChannelOver(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {65, 60, 80}; // Red +25 alone = SAD 25
	}

	static VOID MutateSadSplitEvenlyAtThreshold(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {52, 72, 80}; // +12/+12/+0 = SAD 24
	}

	// SAD == threshold and one under are clean; one over (whole-spread or
	// single-channel) is dirty — the SAD is a sum across all three channels
	static BOOL TestFused_ThresholdBoundary()
	{
		const DirtyRect wholeTile[] = {{0, 0, 64, 64}};
		return RunFusedRectCase(64, 64, 24, &MutateSadExactlyThreshold, nullptr, 0, "ThresholdAt24") &&
		       RunFusedRectCase(64, 64, 24, &MutateSadJustUnder, nullptr, 0, "Threshold23") &&
		       RunFusedRectCase(64, 64, 24, &MutateSadJustOver, wholeTile, 1, "Threshold25") &&
		       RunFusedRectCase(64, 64, 24, &MutateSadSingleChannelOver, wholeTile, 1, "ThresholdSingleChannel25") &&
		       RunFusedRectCase(64, 64, 24, &MutateSadSplitEvenlyAtThreshold, nullptr, 0, "ThresholdSplit24");
	}

	// ---- Single-pixel sensitivity tests ----

	// One interior pixel changed marks exactly its 64x64 tile
	static VOID MutateSinglePixelInterior(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		frame[66 * width + 130] = {240, 200, 160}; // tile (2,1) of a 256x192 frame
	}

	// The row's very last pixel (x = width-1) still lands in the edge tile
	static VOID MutateSinglePixelRightEdge(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		frame[width - 1] = {240, 200, 160}; // tile (21,0) of a 1366-wide frame
	}

	static BOOL TestFused_SinglePixelChange()
	{
		const DirtyRect interior[] = {{128, 64, 64, 64}};
		const DirtyRect rightEdge[] = {{1344, 0, 22, 64}};
		return RunFusedRectCase(256, 192, 24, &MutateSinglePixelInterior, interior, 1, "SinglePixelInterior") &&
		       RunFusedRectCase(1366, 128, 24, &MutateSinglePixelRightEdge, rightEdge, 1, "SinglePixelRightEdge");
	}

	// ---- Edge-clamp shapes beyond the right edge ----

	// Odd widths: the edge tile is 39/37 px wide and must be emitted as-is
	static BOOL TestDirtyRects_OddWidthRightEdge()
	{
		const UINT32 tiles999[] = {15, 0};
		const DirtyRect expected999[] = {{960, 0, 39, 64}};
		const UINT32 tiles997[] = {15, 0};
		const DirtyRect expected997[] = {{960, 0, 37, 64}};
		return RunTwoStepTileCase(999, 128, 64, tiles999, 1, expected999, 1, "OddWidth999") &&
		       RunTwoStepTileCase(997, 128, 64, tiles997, 1, expected997, 1, "OddWidth997");
	}

	// 1280x810: bottom tile row is 42 px tall; a vertical run into it clamps
	static BOOL TestDirtyRects_BottomEdgeRow()
	{
		const UINT32 tilesSingle[] = {10, 12};
		const DirtyRect expectedSingle[] = {{640, 768, 64, 42}};
		const UINT32 tilesMerged[] = {10, 11, 10, 12};
		const DirtyRect expectedMerged[] = {{640, 704, 64, 106}};
		return RunTwoStepTileCase(1280, 810, 64, tilesSingle, 1, expectedSingle, 1, "BottomEdgeSingle") &&
		       RunTwoStepTileCase(1280, 810, 64, tilesMerged, 2, expectedMerged, 1, "BottomEdgeMerged");
	}

	// 1366x810: tile (21,12) clamps in BOTH dimensions; a run into it merges
	static BOOL TestDirtyRects_CornerTileBothEdges()
	{
		const UINT32 tilesCorner[] = {21, 12};
		const DirtyRect expectedCorner[] = {{1344, 768, 22, 42}};
		const UINT32 tilesRun[] = {20, 12, 21, 12};
		const DirtyRect expectedRun[] = {{1280, 768, 86, 42}};
		return RunTwoStepTileCase(1366, 810, 64, tilesCorner, 1, expectedCorner, 1, "CornerTile") &&
		       RunTwoStepTileCase(1366, 810, 64, tilesRun, 2, expectedRun, 1, "CornerRun");
	}

	// Tile size 32 at 1366: the 22-px edge tile's span is exactly the 32-px
	// filter minimum — it must pass (the filter sees the tile span, not the
	// clamped width)
	static BOOL TestDirtyRects_Tile32RightEdge()
	{
		const UINT32 tiles[] = {42, 0};
		const DirtyRect expected[] = {{1344, 0, 22, 32}};
		return RunTwoStepTileCase(1366, 128, 32, tiles, 1, expected, 1, "Tile32RightEdge");
	}

	// ---- Merge shapes and whole-frame coverage ----

	// Tiles (0,0), (1,1), (1,2): the vertical pair must not absorb (0,0)
	// (its span does not match) — two rects in scan order
	static BOOL TestDirtyRects_StaircaseMerge()
	{
		const UINT32 tiles[] = {0, 0, 1, 1, 1, 2};
		const DirtyRect expected[] = {{0, 0, 64, 64}, {64, 64, 64, 128}};
		return RunTwoStepTileCase(192, 192, 64, tiles, 3, expected, 2, "StaircaseMerge");
	}

	// Every pixel dirty: one rect covering the frame, clamped at both edges
	static BOOL TestDirtyRects_FullFrameDirty()
	{
		constexpr UINT32 w = 1366;
		constexpr UINT32 h = 128;
		UINT8 *biDiff = new UINT8[(USIZE)w * h];
		if (!biDiff)
			return false;
		Memory::Set(biDiff, 1, (USIZE)w * h);

		BOOL pass = false;
		auto r = ImageProcessor::FindDirtyRects(Span<const UINT8>(biDiff, (USIZE)w * h), w, h, 64);
		if (!r)
			LOG_ERROR("FullFrameDirty: FindDirtyRects failed: %e", r.Error());
		else
		{
			const DirtyRect expected[] = {{0, 0, 1366, 128}};
			pass = RectsMatch(r.Value(), expected, 1, "FullFrameDirty");
			r.Value().Free();
		}
		delete[] biDiff;
		return pass;
	}

	// 10x10 blocks inside tiles (0,0), (1,1), (1,2)
	static VOID MutateStaircase(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (UINT32 y = 5; y < 15; y++)
			for (UINT32 x = 5; x < 15; x++)
				frame[(USIZE)y * width + x] = {240, 200, 160};
		for (UINT32 y = 69; y < 79; y++)
			for (UINT32 x = 69; x < 79; x++)
				frame[(USIZE)y * width + x] = {240, 200, 160};
		for (UINT32 y = 133; y < 143; y++)
			for (UINT32 x = 69; x < 79; x++)
				frame[(USIZE)y * width + x] = {240, 200, 160};
	}

	static BOOL TestFused_StaircaseRects()
	{
		const DirtyRect expected[] = {{0, 0, 64, 64}, {64, 64, 64, 128}};
		return RunFusedRectCase(192, 192, 24, &MutateStaircase, expected, 2, "FusedStaircase");
	}

	// Block inside the corner tile (21,12) of a 1366x810 frame
	static VOID MutateCornerTile(RGB *frame, UINT32 width, [[maybe_unused]] UINT32 height)
	{
		for (UINT32 y = 774; y < 784; y++)
			for (UINT32 x = 1350; x < 1360; x++)
				frame[(USIZE)y * width + x] = {240, 200, 160};
	}

	static BOOL TestFused_CornerTileRects()
	{
		const DirtyRect expected[] = {{1344, 768, 22, 42}};
		return RunFusedRectCase(1366, 810, 24, &MutateCornerTile, expected, 1, "FusedCornerTile");
	}

	static VOID MutateFullFrame(RGB *frame, UINT32 width, UINT32 height)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {180, 140, 100};
	}

	static BOOL TestFused_FullFrameChange()
	{
		const DirtyRect expected[] = {{0, 0, 1366, 128}};
		return RunFusedRectCase(1366, 128, 24, &MutateFullFrame, expected, 1, "FusedFullFrame");
	}

	// ---- Statelessness across calls ----

	// Restore the flat frame, then dirty 10x10 blocks at the given columns
	// (tile row 1 of a 256x192 frame: x 70..80 is tile (1,1), x 136..146 is (2,1))
	static VOID DirtyRegionBlocks(RGB *frame, UINT32 width, UINT32 height, BOOL left, BOOL right)
	{
		for (USIZE i = 0; i < (USIZE)width * height; i++)
			frame[i] = {40, 60, 80};
		for (UINT32 y = 70; y < 80; y++)
		{
			for (UINT32 x = 70; x < 80 && left; x++)
				frame[(USIZE)y * width + x] = {240, 200, 160};
			for (UINT32 x = 136; x < 146 && right; x++)
				frame[(USIZE)y * width + x] = {240, 200, 160};
		}
	}

	// Disjoint regions dirty on alternating frames: each call's rects reflect
	// only that call's inputs — no state carries between invocations
	static BOOL TestFused_AlternatingRegions()
	{
		constexpr UINT32 w = 256;
		constexpr UINT32 h = 192;
		RGB *frame = nullptr;
		RGB *base = nullptr;
		if (!BuildFramePair(w, h, &frame, &base, nullptr))
			return false;

		BOOL pass = false;
		auto run = [&](PCCHAR label, BOOL left, BOOL right,
		               const DirtyRect *expected, UINT32 expectedCount) -> BOOL {
			DirtyRegionBlocks(frame, w, h, left, right);
			auto r = ImageProcessor::FindDirtyRects(
				Span<RGB>(frame, (USIZE)w * h), Span<const RGB>(base, (USIZE)w * h), w, h, 64, 24);
			if (!r)
			{
				LOG_ERROR("%s: fused FindDirtyRects failed: %e", label, r.Error());
				return false;
			}
			BOOL ok = RectsMatch(r.Value(), expected, expectedCount, label);
			r.Value().Free();
			return ok;
		};

		const DirtyRect leftRect[] = {{64, 64, 64, 64}};
		const DirtyRect rightRect[] = {{128, 64, 64, 64}};
		// The two blocks sit in adjacent tiles, so both dirty at once merge
		// into one horizontal run
		const DirtyRect mergedRect[] = {{64, 64, 128, 64}};
		pass = run("AlternatingLeft", true, false, leftRect, 1) &&
		       run("AlternatingRight", false, true, rightRect, 1) &&
		       run("AlternatingLeftAgain", true, false, leftRect, 1) &&
		       run("AlternatingBoth", true, true, mergedRect, 1);

		delete[] frame;
		delete[] base;
		return pass;
	}

	// A +2/channel fade per capture: four frames stay under the threshold
	// (each reverted against the base), the fifth crosses it
	static BOOL TestFused_DriftAccumulatesMultiFrame()
	{
		const USIZE px = 64 * 64;
		RGB *base = new RGB[px];
		RGB *frame = new RGB[px];
		if (base == nullptr || frame == nullptr)
		{
			delete[] base;
			delete[] frame;
			return false;
		}
		for (USIZE i = 0; i < px; i++)
			base[i] = {10, 10, 10};

		BOOL pass = true;
		// n = 1..4: SAD = 6n <= 24 — clean, and each frame is fully reverted
		for (UINT32 n = 1; n <= 4 && pass; n++)
		{
			UINT8 level = (UINT8)(10 + 2 * n);
			for (USIZE i = 0; i < px; i++)
				frame[i] = {level, level, level};

			auto r = ImageProcessor::FindDirtyRects(
				Span<RGB>(frame, px), Span<const RGB>(base, px), 64, 64, 64, 24);
			if (!r)
			{
				LOG_ERROR("DriftMultiFrame: frame %u pass failed: %e", n, r.Error());
				pass = false;
			}
			else
			{
				if (r.Value().Count != 0)
				{
					LOG_ERROR("DriftMultiFrame: frame %u (SAD %u) reported %u rects", n, 6 * n, r.Value().Count);
					pass = false;
				}
				r.Value().Free();
			}
			for (USIZE i = 0; pass && i < px; i++)
				if (frame[i].Red != 10 || frame[i].Green != 10 || frame[i].Blue != 10)
				{
					LOG_ERROR("DriftMultiFrame: frame %u drift not reverted", n);
					pass = false;
				}
		}

		// n = 5: SAD 30 crosses the threshold against the untouched base
		if (pass)
		{
			for (USIZE i = 0; i < px; i++)
				frame[i] = {20, 20, 20};
			auto r = ImageProcessor::FindDirtyRects(
				Span<RGB>(frame, px), Span<const RGB>(base, px), 64, 64, 64, 24);
			if (!r)
			{
				LOG_ERROR("DriftMultiFrame: crossing pass failed: %e", r.Error());
				pass = false;
			}
			else
			{
				const DirtyRect expected[] = {{0, 0, 64, 64}};
				pass = RectsMatch(r.Value(), expected, 1, "DriftMultiFrameCrossing");
				r.Value().Free();
			}
		}

		delete[] base;
		delete[] frame;
		return pass;
	}
};
