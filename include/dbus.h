/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef DBUS_H
#define DBUS_H

#include "stream.h"

#define IFACE_SC      "io.furios.Mutter.ScreenCast"
#define IFACE_SESSION "io.furios.Mutter.ScreenCast.Session"
#define IFACE_STREAM  "io.furios.Mutter.ScreenCast.Stream"
#define IFACE_PATH    "/io/furios/Mutter/ScreenCast"

/* RequestFrame tick interval */
#define REQUEST_INTERVAL_MS 16

/**
 * Connect to the session bus and watch for IFACE_SC appearance/vanish.
 *
 * @param st  Stream state
 */
void
setup_bus_and_watch(StreamState *st);

/**
 * Stop streaming, unsubscribe signals, unmap/close memfd, and cleanup DRM.
 *
 * @param st  Stream state
 */
void
stream_cleanup(StreamState *st);

/**
 * Full cleanup including name watch and bus.
 *
 * @param st  Stream state
 */
void
cleanup_all(StreamState *st);

/**
 * Re-arm the RequestFrame pacing timer to align with the next predicted vblank.
 *
 * If vblank timing is not yet known, it falls back to REQUEST_INTERVAL_MS pacing.
 *
 * @param st  Stream state
 */
void
stream_rearm_request_timer(StreamState *st);

#endif // DBUS_H
