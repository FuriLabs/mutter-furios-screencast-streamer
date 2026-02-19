CC = gcc

CFLAGS = `pkg-config --cflags gio-2.0 gio-unix-2.0 glib-2.0 libdrm` -Iinclude
LDFLAGS = `pkg-config --libs gio-2.0 gio-unix-2.0 glib-2.0 libdrm`

SOURCES = src/main.c \
          src/dbus.c \
          src/memfd.c \
          src/utils.c \
          src/drm_utils.c \
          src/drm_memfd.c \
          src/drm_native_buffer.c

TARGET = mutter-furios-screencast-streamer

PREFIX ?= /usr

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/libexec
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/libexec/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/libexec/$(TARGET)
