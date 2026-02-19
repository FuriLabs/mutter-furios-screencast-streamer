/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <glib.h>

/**
 * Generic damage rectangle (x/y/w/h).
 *
 * Coordinates are in pixel units with origin at top-left unless otherwise
 * specified by the producer.
 */
typedef struct DamageRect
{
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} DamageRect;

/**
 * Return CLOCK_MONOTONIC time in nanoseconds.
 *
 * @return monotonic time in ns, or 0 on failure
 */
guint64
monotonic_ns(void);

/**
 * Clamp an unsigned 64-bit integer to a range.
 *
 * @param v Value to clamp
 * @param lo Lower bound
 * @param hi Upper bound
 * @return Clamped value
 */
guint64
clamp_u64(guint64 v,
          guint64 lo,
          guint64 hi);

/**
 * Clamp a signed 32-bit integer to a range.
 *
 * @param v Value to clamp
 * @param lo Lower bound
 * @param hi Upper bound
 * @return Clamped value
 */
int32_t
clamp_i32(int32_t v,
          int32_t lo,
          int32_t hi);

/**
 * Arm a timerfd using absolute CLOCK_MONOTONIC nanoseconds.
 *
 * @param fd timerfd file descriptor
 * @param when_ns Absolute time (CLOCK_MONOTONIC) in nanoseconds
 */
void
arm_timerfd_abs_ns(int fd,
                   guint64 when_ns);

/**
 * Clamp and normalize damage rectangles to buffer bounds.
 *
 * @param rects Damage rectangle array (may be NULL)
 * @param w Buffer width in pixels
 * @param h Buffer height in pixels
 */
void
sanitize_damage_rects(GArray   *rects,
                      uint32_t  w,
                      uint32_t  h);

/**
 * Check if damage represents a full-frame update.
 *
 * @param rects Damage rectangle array (may be NULL)
 * @param w Buffer width in pixels
 * @param h Buffer height in pixels
 * @return TRUE if exactly one rectangle covers the entire frame
 */
gboolean
is_full_damage(GArray   *rects,
               uint32_t  w,
               uint32_t  h);

#endif // UTILS_H
