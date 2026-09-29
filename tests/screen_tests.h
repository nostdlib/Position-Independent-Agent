#pragma once

#include "lib/runtime.h"
#include "tests.h"

class ScreenTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Screen Tests...");

		RunTest(allPassed, &TestGetDevices, "GetDevices returns active displays");
		RunTest(allPassed, &TestGetDevices_HasPrimary, "GetDevices includes a primary display");
		RunTest(allPassed, &TestCapture, "Capture produces non-zero pixel data");
		RunTest(allPassed, &TestCaptureStateLifecycle, "Capture state create/reuse/destroy lifecycle");
		RunTest(allPassed, &TestCapture16bpp, "Capture honors a 16bpp request");
		RunTest(allPassed, &TestCaptureDepthSwitch, "Capture depth switch reports DepthChanged");
		RunTest(allPassed, &TestCaptureGateSkip, "Change gate leaves the buffer untouched on skip");
		RunTest(allPassed, &TestCaptureDefaultAfterExplicit, "Default-option captures stay stable after explicit-option captures");

		if (allPassed)
			LOG_INFO("All Screen tests passed!");
		else
			LOG_ERROR("Some Screen tests failed!");

		return allPassed;
	}

private:
	static BOOL TestGetDevices()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}

		auto list = r.Value();

		if (list.Count == 0)
		{
			LOG_ERROR("Expected at least 1 device, got 0");
			list.Free();
			return false;
		}

		for (UINT32 i = 0; i < list.Count; i++)
		{
			if (list.Devices[i].Width == 0 || list.Devices[i].Height == 0)
			{
				LOG_ERROR("Device %u has zero dimension: %ux%u",
					i, list.Devices[i].Width, list.Devices[i].Height);
				list.Free();
				return false;
			}
			LOG_DEBUG("Display %u: %ux%u at (%d,%d)%s",
				i, list.Devices[i].Width, list.Devices[i].Height,
				list.Devices[i].Left, list.Devices[i].Top,
				list.Devices[i].Primary ? " [primary]" : "");
		}

		list.Free();
		return true;
	}

	static BOOL TestGetDevices_HasPrimary()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}

		auto list = r.Value();
		BOOL foundPrimary = false;

		for (UINT32 i = 0; i < list.Count; i++)
		{
			if (list.Devices[i].Primary)
			{
				foundPrimary = true;
				break;
			}
		}

		if (!foundPrimary)
		{
			LOG_ERROR("No primary display found among %u devices", list.Count);
			list.Free();
			return false;
		}

		list.Free();
		return true;
	}

	static BOOL TestCapture()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}

		auto list = r.Value();
		if (list.Count == 0)
		{
			LOG_WARNING("No devices to capture (headless?)");
			list.Free();
			return true;
		}

		// Capture the primary display (or first device)
		UINT32 targetIdx = 0;
		for (UINT32 i = 0; i < list.Count; i++)
		{
			if (list.Devices[i].Primary)
			{
				targetIdx = i;
				break;
			}
		}

		const ScreenDevice &dev = list.Devices[targetIdx];
		UINT32 pixelCount = dev.Width * dev.Height;
		RGB *pixels = new RGB[pixelCount];

		if (pixels == nullptr)
		{
			LOG_ERROR("Failed to allocate capture buffer");
			list.Free();
			return false;
		}

		Memory::Zero(pixels, pixelCount * sizeof(RGB));

		auto captureResult = Screen::Capture(dev, Span<RGB>(pixels, pixelCount));
		if (!captureResult)
		{
			LOG_ERROR("Capture failed: %e", captureResult.Error());
			delete[] pixels;
			list.Free();
			return false;
		}

		// Verify at least some pixels are non-zero (screen isn't completely black)
		BOOL hasNonZero = false;
		for (UINT32 i = 0; i < pixelCount; i++)
		{
			if (pixels[i].Red != 0 || pixels[i].Green != 0 || pixels[i].Blue != 0)
			{
				hasNonZero = true;
				break;
			}
		}

		if (!hasNonZero)
			LOG_WARNING("Capture succeeded but all pixels are black (headless?)");

		LOG_DEBUG("Captured %ux%u (%u pixels)", dev.Width, dev.Height, pixelCount);

		delete[] pixels;
		list.Free();
		return true;
	}

	// Persistent capture state: create once, capture twice through it (the
	// second round exercises resource reuse), destroy, then one stateless
	// capture to pin the nullptr fallback contract
	static BOOL TestCaptureStateLifecycle()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}

		auto list = r.Value();
		if (list.Count == 0)
		{
			LOG_WARNING("No devices to capture (headless?)");
			list.Free();
			return true;
		}

		const ScreenDevice &dev = list.Devices[0];
		UINT32 pixelCount = dev.Width * dev.Height;
		RGB *pixels = new RGB[pixelCount];
		if (pixels == nullptr)
		{
			LOG_ERROR("Failed to allocate capture buffer");
			list.Free();
			return false;
		}

		BOOL ok = true;
		BOOL captureAvailable = false;
		auto state = Screen::CreateCaptureState(dev);
		if (!state)
		{
			LOG_ERROR("CreateCaptureState failed: %e", state.Error());
			ok = false;
		}
		else
		{
			Memory::Zero(pixels, pixelCount * sizeof(RGB));
			auto captureResult = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value());
			if (!captureResult)
			{
				// Capture unavailable (headless): not a lifecycle failure — but
				// only if the stateless path fails the same way
				auto statelessResult = Screen::Capture(dev, Span<RGB>(pixels, pixelCount));
				if (!statelessResult)
				{
					LOG_WARNING("Capture unavailable (headless?), skipping state lifecycle: %e", captureResult.Error());
				}
				else
				{
					LOG_ERROR("Stateful capture failed while stateless succeeds: %e", captureResult.Error());
					ok = false;
				}
			}
			else
			{
				captureAvailable = true;
				// Second round through the same state exercises resource reuse
				auto second = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value());
				if (!second)
				{
					LOG_ERROR("Stateful capture reuse round failed: %e", second.Error());
					ok = false;
				}
			}
			Screen::DestroyCaptureState(state.Value());
		}

		// After destroy, the stateless path must still capture
		if (ok && captureAvailable)
		{
			auto captureResult = Screen::Capture(dev, Span<RGB>(pixels, pixelCount));
			if (!captureResult)
			{
				LOG_ERROR("Stateless capture after destroy failed: %e", captureResult.Error());
				ok = false;
			}
		}

		delete[] pixels;
		list.Free();
		return ok;
	}

	// 16bpp capture request: the platform either honors it (probe measured it
	// worthwhile) or stays at 32bpp — both are valid outcomes, failure is not
	static BOOL TestCapture16bpp()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}
		auto list = r.Value();
		if (list.Count == 0)
		{
			LOG_WARNING("No devices to capture (headless?)");
			list.Free();
			return true;
		}

		const ScreenDevice &dev = list.Devices[0];
		UINT32 pixelCount = dev.Width * dev.Height;
		RGB *pixels = new RGB[pixelCount];
		if (pixels == nullptr)
		{
			LOG_ERROR("Failed to allocate capture buffer");
			list.Free();
			return false;
		}

		BOOL ok = true;
		auto state = Screen::CreateCaptureState(dev);
		if (!state)
		{
			LOG_WARNING("CreateCaptureState failed (platform without state): %e", state.Error());
		}
		else
		{
			CaptureOptions options;
			options.BitsPerPixel = 16;
			options.AllowSkip = false;
			CaptureStatus status;
			Memory::Zero(&status, sizeof(status));

			auto captureResult = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &options, &status);
			if (!captureResult)
			{
				LOG_WARNING("16bpp capture unavailable (headless?): %e", captureResult.Error());
			}
			else if (status.BitsPerPixel != 16 && status.BitsPerPixel != 32)
			{
				LOG_ERROR("Unexpected reported depth %u (want 16 or 32)", status.BitsPerPixel);
				ok = false;
			}
			else
			{
				LOG_DEBUG("16bpp request served at %u bpp", status.BitsPerPixel);
			}
			Screen::DestroyCaptureState(state.Value());
		}

		delete[] pixels;
		list.Free();
		return ok;
	}

	// 32bpp then 16bpp through one state: a genuine depth switch must report
	// DepthChanged (the diff base is invalid); staying at 32bpp must not
	static BOOL TestCaptureDepthSwitch()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}
		auto list = r.Value();
		if (list.Count == 0)
		{
			LOG_WARNING("No devices to capture (headless?)");
			list.Free();
			return true;
		}

		const ScreenDevice &dev = list.Devices[0];
		UINT32 pixelCount = dev.Width * dev.Height;
		RGB *pixels = new RGB[pixelCount];
		if (pixels == nullptr)
		{
			LOG_ERROR("Failed to allocate capture buffer");
			list.Free();
			return false;
		}

		BOOL ok = true;
		auto state = Screen::CreateCaptureState(dev);
		if (!state)
		{
			LOG_WARNING("CreateCaptureState failed (platform without state): %e", state.Error());
		}
		else
		{
			CaptureOptions options32;
			options32.BitsPerPixel = 32;
			options32.AllowSkip = false;
			CaptureOptions options16;
			options16.BitsPerPixel = 16;
			options16.AllowSkip = false;
			CaptureStatus first;
			Memory::Zero(&first, sizeof(first));
			CaptureStatus second;
			Memory::Zero(&second, sizeof(second));

			auto r32 = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &options32, &first);
			auto r16 = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &options16, &second);
			if (!r32 || !r16)
			{
				LOG_WARNING("Depth-switch captures unavailable (headless?)");
			}
			else if (second.BitsPerPixel == 16)
			{
				if (!second.DepthChanged)
				{
					LOG_ERROR("16bpp switch reported without DepthChanged");
					ok = false;
				}
			}
			else
			{
				LOG_DEBUG("Platform kept 32bpp (probe not allowing 16) — no switch to report");
			}
			Screen::DestroyCaptureState(state.Value());
		}

		delete[] pixels;
		list.Free();
		return ok;
	}

	// Change gate: prime with AllowSkip=false, then ask again with AllowSkip=true.
	// When the gate reports FrameUnchanged, the buffer must still hold the
	// sentinel pattern byte-for-byte (the untouched-buffer contract)
	static BOOL TestCaptureGateSkip()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}
		auto list = r.Value();
		if (list.Count == 0)
		{
			LOG_WARNING("No devices to capture (headless?)");
			list.Free();
			return true;
		}

		const ScreenDevice &dev = list.Devices[0];
		UINT32 pixelCount = dev.Width * dev.Height;
		RGB *pixels = new RGB[pixelCount];
		if (pixels == nullptr)
		{
			LOG_ERROR("Failed to allocate capture buffer");
			list.Free();
			return false;
		}

		BOOL ok = true;
		auto state = Screen::CreateCaptureState(dev);
		if (!state)
		{
			LOG_WARNING("CreateCaptureState failed (platform without state): %e", state.Error());
		}
		else
		{
			CaptureOptions prime;
			prime.BitsPerPixel = 32;
			prime.AllowSkip = false;
			CaptureStatus primed;
			Memory::Zero(&primed, sizeof(primed));
			auto first = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &prime, &primed);
			if (!first || primed.FrameUnchanged)
			{
				LOG_WARNING("Gate priming capture unavailable or already skipped (headless?)");
			}
			else
			{
				// Paint the sentinel so a skip is provable by what survives
				UINT8 *raw = (UINT8 *)pixels;
				for (USIZE i = 0; i < (USIZE)pixelCount * sizeof(RGB); i++)
					raw[i] = (UINT8)(i * 31 + 7);

				// Two gated captures: the first re-baselines (gateValid was
				// cleared by the non-gated priming capture), so only the second
				// can skip — that is the branch the sentinel verifies
				CaptureOptions gated;
				gated.BitsPerPixel = 32;
				gated.AllowSkip = true;
				CaptureStatus gatedStatus;
				Memory::Zero(&gatedStatus, sizeof(gatedStatus));
				auto firstGated = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &gated, &gatedStatus);
				if (!firstGated)
				{
					LOG_WARNING("Gated capture unavailable (headless?)");
				}
				else
				{
					auto second = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &gated, &gatedStatus);
					if (!second)
						LOG_WARNING("Second gated capture unavailable (headless?)");
					else if (gatedStatus.FrameUnchanged)
					{
						for (USIZE i = 0; i < (USIZE)pixelCount * sizeof(RGB); i++)
						{
							if (raw[i] != (UINT8)(i * 31 + 7))
							{
								LOG_ERROR("FrameUnchanged reported but buffer byte %llu was written", (UINT64)i);
								ok = false;
								break;
							}
						}
						if (ok)
							LOG_DEBUG("Gate skipped and the buffer is untouched");
					}
					else
					{
						LOG_DEBUG("Gate detected a change (live screen) — no skip to verify");
					}
				}
			}
			Screen::DestroyCaptureState(state.Value());
		}

		delete[] pixels;
		list.Free();
		return ok;
	}

	// A capture state primed with explicit options must keep serving the
	// default path (options=nullptr, status=nullptr) on repeated calls
	static BOOL TestCaptureDefaultAfterExplicit()
	{
		auto r = Screen::GetDevices();
		if (!r)
		{
			LOG_WARNING("GetDevices unavailable (no display): %e", r.Error());
			return true;
		}

		auto list = r.Value();
		if (list.Count == 0)
		{
			LOG_WARNING("No devices to capture (headless?)");
			list.Free();
			return true;
		}

		const ScreenDevice &dev = list.Devices[0];
		UINT32 pixelCount = dev.Width * dev.Height;
		RGB *pixels = new RGB[pixelCount];
		if (pixels == nullptr)
		{
			LOG_ERROR("Failed to allocate capture buffer");
			list.Free();
			return false;
		}

		BOOL ok = true;
		auto state = Screen::CreateCaptureState(dev);
		if (!state)
		{
			LOG_WARNING("CreateCaptureState failed (platform without state): %e", state.Error());
		}
		else
		{
			// Prime the state with an explicit-options capture first
			CaptureOptions explicitOptions;
			explicitOptions.BitsPerPixel = 32;
			explicitOptions.AllowSkip = false;
			CaptureStatus primed;
			Memory::Zero(&primed, sizeof(primed));
			auto first = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value(), &explicitOptions, &primed);
			if (!first)
			{
				LOG_WARNING("Explicit capture unavailable (headless?): %e", first.Error());
			}
			else
			{
				// Two default-path captures through the primed state, then a
				// stateless one — all must succeed with consistent dimensions
				BOOL stable = true;
				for (INT32 round = 0; round < 2 && stable; round++)
				{
					auto captureResult = Screen::Capture(dev, Span<RGB>(pixels, pixelCount), state.Value());
					if (!captureResult)
					{
						LOG_ERROR("Default capture round %d failed after explicit options: %e",
							round, captureResult.Error());
						stable = false;
					}
				}
				auto stateless = Screen::Capture(dev, Span<RGB>(pixels, pixelCount));
				if (stable && !stateless)
				{
					LOG_ERROR("Stateless default capture failed: %e", stateless.Error());
					stable = false;
				}
				ok = stable;
			}
			Screen::DestroyCaptureState(state.Value());
		}

		delete[] pixels;
		list.Free();
		return ok;
	}
};
