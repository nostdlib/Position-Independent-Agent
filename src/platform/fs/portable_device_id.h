/**
 * @file portable_device_id.h
 * @brief WPDBusEnum volume-wrapper detection
 *
 * @details Classifies a WPD PnP device id as a WPDBusEnum volume wrapper
 * (`\\?\swd#wpdbusenum#…`) — the alternate face of a mounted removable
 * mass-storage volume that the root listing's drive rows already cover.
 * The enumerator prefix is the discriminator because the wrapper id is an
 * opaque {guid}: hardware-verified on Win11 26300 that it carries no serial
 * or inquiry identity to correlate against.
 *
 * Lives in the platform layer (not fs/windows) as a pure string predicate
 * so the test binary — which does not compile fs/windows/wpd.cc on Linux
 * hosts — compiles and golden-vector-tests it on every host (the
 * wire_listing.h precedent).
 */

#pragma once

#include "core/types/primitives.h"
#include "core/string/string.h"

/// TRUE when the device id is a WPDBusEnum volume wrapper: that enumerator
/// (swd\wpdbusenum) creates devices as alternate faces of mounted removable
/// mass-storage volumes (hardware-validated for volume wrappers on Win11
/// 26300; whether any other device class ever appears there is unvalidated).
/// The wrapper id carries an opaque {guid} — no serial, no inquiry identity —
/// so the enumerator prefix is the discriminator. Portable devices are
/// expected on their own buses (usb#vid_…); one appearing under wpdbusenum
/// too would merely inflate a wrapper count, which safely turns count-based
/// suppression OFF (a duplicate listed, never a device hidden).
inline BOOL IsVolumePersonaId(PCWCHAR deviceId)
{
	if (deviceId == nullptr)
		return false;
	// GetDevices ids carry the \\?\ device-interface prefix; strip it so the
	// check is exactly "the first two #-segments name swd and wpdbusenum".
	if (deviceId[0] == L'\\' && deviceId[1] == L'\\' && deviceId[2] == L'?' && deviceId[3] == L'\\')
		deviceId += 4;
	const WCHAR prefix[] = L"swd#wpdbusenum";
	USIZE i = 0;
	for (; prefix[i] != L'\0'; i++)
		if (deviceId[i] == L'\0' || StringUtils::ToLowerCase(deviceId[i]) != prefix[i])
			return false;
	// The next character must end the wpdbusenum segment — a longer segment
	// ("wpdbusenumfoo") is a different enumerator.
	return deviceId[i] == L'#' || deviceId[i] == L'\0';
}
