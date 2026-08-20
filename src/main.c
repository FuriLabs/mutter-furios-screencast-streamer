/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <gio/gio.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "dbus.h"
#include "memfd.h"
#include "stream.h"

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
  g_print("usage: %s [--card N] [--connector NAME] [--backend BACKEND]\n"
          "  --card N         DRM card index (card1 => 1). default: 1\n"
          "  --connector NAME Connector name like DVI-I-1, DP-1, HDMI-A-1 (optional)\n"
          "  --backend NAME   Stream backend: auto, memfd, native-buffer. default: auto\n"
          "\nexample:\n"
          "  %s --card 1 --connector DVI-I-1 --backend native-buffer\n",
          argv0,
          argv0);
}

int
main(int argc, char **argv)
{
  StreamState st;

  memset(&st, 0, sizeof(st));

  st.memfd.fd = -1;
  st.sink.card_index = 1;
  st.sink.connector_want = NULL;
  st.sink.drm_fd = -1;

  st.request_timer_fd = -1;
  st.request_timer_source_id = 0;

  st.vblank_valid = FALSE;
  st.vblank_last_ns = 0;
  st.vblank_period_ns = 0;
  st.vblank_lead_ns = 2000000;

  st.backend_override = STREAM_BACKEND_UNKNOWN;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--card") == 0 && i + 1 < argc) {
      st.sink.card_index = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--connector") == 0 && i + 1 < argc) {
      st.sink.connector_want = g_strdup(argv[++i]);
    } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
      const char *backend = argv[++i];

      if (strcmp(backend, "auto") == 0) {
        st.backend_override = STREAM_BACKEND_UNKNOWN;
      } else if (strcmp(backend, "memfd") == 0) {
        st.backend_override = STREAM_BACKEND_MEMFD;
      } else if (strcmp(backend, "native-buffer") == 0) {
        st.backend_override = STREAM_BACKEND_NATIVE_BUFFER;
      } else {
        g_printerr("invalid backend: %s\n", backend);
        usage(argv[0]);
        return 2;
      }
    } else if (strcmp(argv[i], "--help") == 0 ||
               strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  const char *backend_str = (st.backend_override == STREAM_BACKEND_MEMFD) ? "memfd" :
                            (st.backend_override == STREAM_BACKEND_NATIVE_BUFFER) ? "native-buffer" :
                            "auto";

  g_print("[args] /dev/dri/card%d connector=%s backend=%s\n",
          st.sink.card_index,
          (st.sink.connector_want && *st.sink.connector_want)
            ? st.sink.connector_want
            : "(auto)",
          backend_str);

  setup_bus_and_watch(&st);

  while (!g_stop)
    g_main_context_iteration(NULL, TRUE);

  cleanup_all(&st);
  g_free(st.sink.connector_want);

  return 0;
}
