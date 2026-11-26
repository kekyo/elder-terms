// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-background-image.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cstring>
#include <new>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr std::size_t image_limit = 64 * 1024 * 1024;

struct ImageRead {
  std::string path;
  GFile *file;
  GCancellable *cancel;
  std::function<void(TerminalBackgroundImagePtr, std::string)> complete;
  GFileInputStream *stream = nullptr;
  GdkPixbufLoader *loader = nullptr;
  std::size_t received = 0;
  bool oversized = false;
  bool loader_closed = false;
  TerminalBackgroundImagePtr image;
  std::string error;
};

static bool dimensions_valid(int width, int height) {
  return width > 0 && height > 0 && width <= 16384 && height <= 16384 &&
      static_cast<std::size_t>(width) <= image_limit / 4 / static_cast<std::size_t>(height);
}

static void release_read(ImageRead *read) {
  auto complete = std::move(read->complete);
  auto image = std::move(read->image);
  auto error = std::move(read->error);
  if (read->loader) {
    if (!read->loader_closed) gdk_pixbuf_loader_close(read->loader, nullptr);
    g_object_unref(read->loader);
  }
  g_clear_object(&read->stream);
  g_object_unref(read->file);
  g_object_unref(read->cancel);
  delete read;
  complete(std::move(image), std::move(error));
}

static void finish_read(ImageRead *read, TerminalBackgroundImagePtr image, std::string error) {
  read->image = std::move(image);
  read->error = std::move(error);
  if (!read->stream) { release_read(read); return; }
  // The read callback has finished; close even after cancellation before
  // relinquishing the stream, without blocking GTK on file-system I/O.
  g_input_stream_close_async(G_INPUT_STREAM(read->stream), G_PRIORITY_DEFAULT, nullptr,
      +[](GObject *stream, GAsyncResult *result, gpointer data) {
        g_input_stream_close_finish(G_INPUT_STREAM(stream), result, nullptr);
        release_read(static_cast<ImageRead *>(data));
      }, read);
}

static void fail_read(ImageRead *read, GError *error) {
  const std::string message = error ? error->message : _("Cannot decode the background image");
  g_clear_error(&error);
  finish_read(read, nullptr, message);
}

static TerminalBackgroundImagePtr copy_pixels(ImageRead *read) {
  auto *pixbuf = gdk_pixbuf_loader_get_pixbuf(read->loader);
  if (!pixbuf || read->oversized) return nullptr;
  const auto width = gdk_pixbuf_get_width(pixbuf);
  const auto height = gdk_pixbuf_get_height(pixbuf);
  const auto channels = gdk_pixbuf_get_n_channels(pixbuf);
  const auto stride = gdk_pixbuf_get_rowstride(pixbuf);
  if (!dimensions_valid(width, height) || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
      gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB || (channels != 3 && channels != 4) || stride <= 0)
    return nullptr;
  const auto row_stride = static_cast<std::size_t>(stride);
  const auto row_bytes = static_cast<std::size_t>(width) * channels;
  if (row_stride < row_bytes || row_stride > image_limit / height ||
      gdk_pixbuf_get_byte_length(pixbuf) < static_cast<std::size_t>(height - 1) * row_stride + row_bytes)
    return nullptr;
  auto image = std::make_shared<TerminalBackgroundImage>(TerminalBackgroundImage{read->path,
      static_cast<std::size_t>(width), static_cast<std::size_t>(height), {}});
  image->rgba.resize(image->width * image->height * 4);
  const auto *pixels = gdk_pixbuf_read_pixels(pixbuf);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto *source = pixels + static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * channels;
      auto *target = image->rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      std::memcpy(target, source, 3);
      target[3] = channels == 4 ? source[3] : 255;
    }
  }
  return image;
}

static void read_chunk(ImageRead *read);

static void chunk_ready(GObject *stream, GAsyncResult *result, gpointer data) {
  auto *read = static_cast<ImageRead *>(data);
  GError *error = nullptr;
  auto *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(stream), result, &error);
  if (!bytes) { fail_read(read, error); return; }
  gsize length = 0;
  const auto *buffer = static_cast<const guint8 *>(g_bytes_get_data(bytes, &length));
  if (length == 0) {
    g_bytes_unref(bytes);
    if (!read->loader) { fail_read(read, nullptr); return; }
    const auto decoded = gdk_pixbuf_loader_close(read->loader, &error);
    read->loader_closed = true;
    if (!decoded) { fail_read(read, error); return; }
    try {
      auto image = copy_pixels(read);
      if (!image) { fail_read(read, nullptr); return; }
      finish_read(read, std::move(image), {});
    } catch (const std::bad_alloc &) {
      finish_read(read, nullptr, _("Not enough memory for the background image"));
    }
    return;
  }
  if (length > image_limit - read->received) {
    g_bytes_unref(bytes);
    finish_read(read, nullptr, _("Background image exceeds the 64 MiB limit"));
    return;
  }
  read->received += length;
  if (!read->loader) {
    const char *format = length >= 8 && std::memcmp(buffer, "\x89PNG\r\n\x1a\n", 8) == 0 ? "png" :
        length >= 3 && buffer[0] == 0xff && buffer[1] == 0xd8 && buffer[2] == 0xff ? "jpeg" : nullptr;
    if (!format) {
      g_bytes_unref(bytes);
      finish_read(read, nullptr, _("Select a PNG or JPEG background image"));
      return;
    }
    read->loader = gdk_pixbuf_loader_new_with_type(format, &error);
    if (!read->loader) { g_bytes_unref(bytes); fail_read(read, error); return; }
    g_signal_connect(read->loader, "size-prepared", G_CALLBACK(+[](GdkPixbufLoader *loader, gint width, gint height, gpointer data) {
      auto *read = static_cast<ImageRead *>(data);
      if (!dimensions_valid(width, height)) {
        read->oversized = true;
        // Reject the image, and prevent the decoder from allocating its full
        // output before write() returns and the loader can be closed safely.
        gdk_pixbuf_loader_set_size(loader, 1, 1);
      }
    }), read);
  }
  const auto accepted = gdk_pixbuf_loader_write(read->loader, buffer, length, &error);
  g_bytes_unref(bytes);
  if (read->oversized) {
    g_clear_error(&error);
    finish_read(read, nullptr, _("Background image exceeds 16384 pixels or 64 MiB"));
  } else if (!accepted) fail_read(read, error);
  else read_chunk(read);
}

static void read_chunk(ImageRead *read) {
  g_input_stream_read_bytes_async(G_INPUT_STREAM(read->stream), 64 * 1024, G_PRIORITY_DEFAULT,
      read->cancel, chunk_ready, read);
}

void load_terminal_background_image(std::string path, GCancellable *cancellation,
    std::function<void(TerminalBackgroundImagePtr, std::string)> complete) {
  if (path.empty()) { complete(nullptr, {}); return; }
  if (path.find('\0') != std::string::npos || !g_utf8_validate(path.c_str(), -1, nullptr) || !g_path_is_absolute(path.c_str())) {
    complete(nullptr, _("Enter an absolute local image path"));
    return;
  }
  auto *filename = g_filename_from_utf8(path.c_str(), -1, nullptr, nullptr, nullptr);
  if (!filename) { complete(nullptr, _("Enter an absolute local image path")); return; }
  auto *file = g_file_new_for_path(filename);
  g_free(filename);
  auto *read = new ImageRead{std::move(path), file,
      cancellation ? G_CANCELLABLE(g_object_ref(cancellation)) : g_cancellable_new(), std::move(complete),
      nullptr, nullptr, 0, false, false, {}, {}};
  g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, read->cancel,
      +[](GObject *file, GAsyncResult *result, gpointer data) {
        auto *read = static_cast<ImageRead *>(data);
        GError *error = nullptr;
        auto *info = g_file_query_info_finish(G_FILE(file), result, &error);
        if (!info) { fail_read(read, error); return; }
        const auto regular = g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR;
        const auto size = g_file_info_get_size(info);
        g_object_unref(info);
        if (!regular || size < 0 || static_cast<guint64>(size) > image_limit) {
          finish_read(read, nullptr, !regular ? _("Select a regular local image file") :
              _("Background image exceeds the 64 MiB limit"));
          return;
        }
        g_file_read_async(read->file, G_PRIORITY_DEFAULT, read->cancel,
            +[](GObject *file, GAsyncResult *result, gpointer data) {
              auto *read = static_cast<ImageRead *>(data);
              GError *error = nullptr;
              read->stream = g_file_read_finish(G_FILE(file), result, &error);
              if (!read->stream) fail_read(read, error);
              else read_chunk(read);
            }, read);
      }, read);
}
}
