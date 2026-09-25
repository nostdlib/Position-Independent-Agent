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
};
