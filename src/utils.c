/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "utils.h"

#include <sys/timerfd.h>

guint64
monotonic_ns(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;

  return (guint64)ts.tv_sec * 1000000000ull + (guint64)ts.tv_nsec;
}

guint64
clamp_u64(guint64 v,
          guint64 lo,
          guint64 hi)
{
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

int32_t
clamp_i32(int32_t v,
          int32_t lo,
          int32_t hi)
{
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

void
arm_timerfd_abs_ns(int fd,
                   guint64 when_ns)
{
  struct itimerspec its;

  memset(&its, 0, sizeof(its));

  its.it_value.tv_sec = (time_t)(when_ns / 1000000000ull);
  its.it_value.tv_nsec = (long)(when_ns % 1000000000ull);

  if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) != 0)
    g_warning("timerfd_settime failed: %s", g_strerror(errno));
}

void
sanitize_damage_rects(GArray   *rects,
                      uint32_t  w,
                      uint32_t  h)
{
  if (!rects)
    return;

  if (rects->len == 0) {
    DamageRect r;

    r.x = 0;
    r.y = 0;
    r.w = (int32_t)w;
    r.h = (int32_t)h;
    g_array_append_val(rects, r);
    return;
  }

  for (guint i = 0; i < rects->len; i++) {
    DamageRect *r = &g_array_index(rects, DamageRect, i);

    int32_t x1 = r->x;
    int32_t y1 = r->y;
    int32_t x2 = r->x + r->w;
    int32_t y2 = r->y + r->h;

    x1 = clamp_i32(x1, 0, (int32_t)w);
    y1 = clamp_i32(y1, 0, (int32_t)h);
    x2 = clamp_i32(x2, 0, (int32_t)w);
    y2 = clamp_i32(y2, 0, (int32_t)h);

    r->x = x1;
    r->y = y1;
    r->w = x2 - x1;
    r->h = y2 - y1;

    if (r->w < 0)
      r->w = 0;
    if (r->h < 0)
      r->h = 0;
  }
}

gboolean
is_full_damage(GArray   *rects,
               uint32_t  w,
               uint32_t  h)
{
  if (!rects)
    return FALSE;

  if (rects->len != 1)
    return FALSE;

  DamageRect r = g_array_index(rects, DamageRect, 0);

  if (r.x != 0)
    return FALSE;
  if (r.y != 0)
    return FALSE;
  if ((uint32_t)r.w != w)
    return FALSE;
  if ((uint32_t)r.h != h)
    return FALSE;

  return TRUE;
}
