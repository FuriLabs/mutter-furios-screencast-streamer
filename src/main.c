/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <gio/gio.h>

#include "memfd.h"
#include "drm.h"
#include "stream.h"
#include "dbus.h"

static volatile sig_atomic_t g_stop = 0;

static void
on_sig(int sig)
{
  (void)sig;
  g_stop = 1;
}

static void
usage(const char *argv0)
{
  g_print("usage: %s [--card N] [--connector NAME]\n"
          "  --card N         DRM card index (card1 => 1). default: 1\n"
          "  --connector NAME Connector name like DVI-I-1, DP-1, HDMI-A-1 (optional)\n"
          "\nexample:\n"
          "  %s --card 1 --connector DVI-I-1\n",
          argv0, argv0);
}

int
main(int argc, char **argv)
{
  StreamState st;
  memset(&st, 0, sizeof(st));

  st.memfd = -1;
  st.sink.card_index = 1;
  st.sink.connector_want = NULL;
  st.sink.drm_fd = -1;

  st.request_timer_fd = -1;
  st.request_timer_source_id = 0;

  st.vblank_valid = FALSE;
  st.vblank_last_ns = 0;
  st.vblank_period_ns = 0;
  st.vblank_lead_ns = 2000000;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--card") == 0 && i + 1 < argc) {
      st.sink.card_index = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--connector") == 0 && i + 1 < argc) {
      st.sink.connector_want = g_strdup(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  g_print("[args] /dev/dri/card%d connector=%s\n",
          st.sink.card_index,
          (st.sink.connector_want && *st.sink.connector_want)
            ? st.sink.connector_want
            : "(auto)");

  setup_bus_and_watch(&st);

  while (!g_stop)
    g_main_context_iteration(NULL, TRUE);

  cleanup_all(&st);
  g_free(st.sink.connector_want);
  return 0;
}
