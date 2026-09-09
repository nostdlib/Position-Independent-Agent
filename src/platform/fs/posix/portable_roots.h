/**
 * @file portable_roots.h
 * @brief Linux Portable-Device Mount Root Collection
 *
 * @details Collects pseudo-drive roots for MTP/PTP portable devices (phones,
 * cameras) on Linux so they appear in the empty-path root listing:
 * - udisks-style media mounts: depth-2 directories under /media and
 *   /run/media (`/media/<user>/<label>/`),
 * - GVFS FUSE mounts: `/run/user/<uid>/gvfs/mtp:*` and `/run/user/<uid>/gvfs/
 *   gphoto2:*` (GVfs also exposes other schemes there; only device schemes
 *   are listed).
 *
 * The emitted entries are REAL paths (with a trailing `/`), so browsing and
 * reading them flows through the ordinary POSIX DirectoryIterator/File layer
 * unchanged. Stale or vanished mounts simply fail like any other missing
 * path. Only Linux is supported; other POSIX platforms collect nothing.
 *
 * @note GVFS mount names carry the device identity (`mtp:host=...`), so the
 * entry path is stable across reboots for the same device.
 */

#pragma once

#include "core/types/primitives.h"
#include "core/containers/vector.h"
#include "platform/fs/directory_entry.h"

/**
 * @brief Collector for portable-device mount roots (Linux).
 *
 * @details All collectors are best-effort: unreadable parents (permission,
 * missing gvfs dir) contribute zero entries silently, and allocation failure
 * inside Vector::Add simply drops the surplus entries.
 */
class PortableRoots
{
public:
	/**
	 * @brief Collects every portable-device root of the running user.
	 * @details Combines udisks media mounts (/media, /run/media) with GVFS
	 * device mounts (/run/user). Intended for the empty-path root listing
	 * after the real `/` entries.
	 *
	 * @param out Vector receiving one DirectoryEntry per mount (cleared never;
	 * appended only).
	 */
	static VOID CollectPortableRoots(Vector<DirectoryEntry> &out);

	/**
	 * @brief Collects depth-2 removable-media mounts under a media directory.
	 * @details Enumerates `<mediaDir>/<user>/<label>/` (e.g. the udisks
	 * `/media/<user>/<label>` layout). Iterates with DirectoryIterator.
	 *
	 * @param mediaDir UTF-8 media parent directory (e.g. "/media").
	 * @param out Vector receiving the mount entries.
	 */
	static VOID CollectMountedMedia(const CHAR *mediaDir, Vector<DirectoryEntry> &out);

	/**
	 * @brief Collects GVFS device mounts under a /run/user directory.
	 * @details Enumerates `<runUserDir>/<numeric-uid>/gvfs/` and appends the
	 * `mtp:*` and `gphoto2:*` children. Directories of other users fail with
	 * EACCES and are skipped silently; non-numeric uid names are ignored.
	 *
	 * @param runUserDir UTF-8 parent directory (e.g. "/run/user").
	 * @param out Vector receiving the mount entries.
	 */
	static VOID CollectGvfsMounts(const CHAR *runUserDir, Vector<DirectoryEntry> &out);
};
