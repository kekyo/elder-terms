// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-icon.h>
#include <elder-terms/connection-icon-presets.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr std::size_t image_limit = 64 * 1024 * 1024;
static constexpr int icon_size = 256;
static constexpr int maximum_image_side = 16384;

struct IconDecodeState {
  bool oversized = false;
};

static void on_size_prepared(GdkPixbufLoader *loader, int width, int height,
                             gpointer data) {
  auto *state = static_cast<IconDecodeState *>(data);
  if (width <= 0 || height <= 0 || width > maximum_image_side ||
      height > maximum_image_side ||
      static_cast<std::size_t>(width) > image_limit / 4 /
                                          static_cast<std::size_t>(height)) {
    state->oversized = true;
    gdk_pixbuf_loader_set_size(loader, 1, 1);
    return;
  }
  const auto factor = std::min(1.0, static_cast<double>(icon_size) /
                                       static_cast<double>(std::max(width, height)));
  const int scaled_width = std::max(1, static_cast<int>(std::round(width * factor)));
  const int scaled_height = std::max(1, static_cast<int>(std::round(height * factor)));
  gdk_pixbuf_loader_set_size(loader, scaled_width, scaled_height);
}

static std::shared_ptr<GdkPixbuf> square_image(GdkPixbuf *source) {
  if (source == nullptr || gdk_pixbuf_get_width(source) <= 0 ||
      gdk_pixbuf_get_height(source) <= 0) {
    return nullptr;
  }
  const int source_width = gdk_pixbuf_get_width(source);
  const int source_height = gdk_pixbuf_get_height(source);
  const auto factor = static_cast<double>(icon_size) /
                      static_cast<double>(std::max(source_width, source_height));
  const int width = std::max(1, static_cast<int>(std::round(source_width * factor)));
  const int height = std::max(1, static_cast<int>(std::round(source_height * factor)));
  auto square = std::shared_ptr<GdkPixbuf>(
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, icon_size, icon_size),
      g_object_unref);
  if (square == nullptr) return nullptr;
  gdk_pixbuf_fill(square.get(), 0);
  auto scaled = std::shared_ptr<GdkPixbuf>(
      gdk_pixbuf_scale_simple(source, width, height, GDK_INTERP_BILINEAR),
      g_object_unref);
  if (scaled == nullptr) return nullptr;
  gdk_pixbuf_copy_area(scaled.get(), 0, 0, width, height, square.get(),
                       (icon_size - width) / 2, (icon_size - height) / 2);
  return square;
}

std::shared_ptr<GdkPixbuf> default_connection_icon() {
  static const auto cached = [] {
    GError *error = nullptr;
    auto *raw = gdk_pixbuf_new_from_resource(
        "/net/kekyo/elder-terms/icons/256x256.png", &error);
    g_clear_error(&error);
    if (raw == nullptr) return std::shared_ptr<GdkPixbuf>{};
    auto original = std::shared_ptr<GdkPixbuf>(raw, g_object_unref);
    return square_image(original.get());
  }();
  return cached;
}

static ConnectionIconLoadResult fallback_icon(const char *reason) {
  return {default_connection_icon(), true, reason};
}

static const char *image_type(const guint8 *bytes, gsize length) {
  if (length >= 8 && std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
    return "png";
  if (length >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
      bytes[2] == 0xff)
    return "jpeg";
  return nullptr;
}

static void validate_image_file_info(GFileInfo *info) {
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR)
    throw std::runtime_error(_("Select a regular local icon file"));
  if (g_file_info_get_size(info) < 0 ||
      static_cast<guint64>(g_file_info_get_size(info)) > image_limit)
    throw std::runtime_error(_("Icon image exceeds the 64 MiB limit"));
}

static cardio::promise<std::shared_ptr<GdkPixbuf>> read_icon_file_async(
    std::string path, cardio::cancellation cancellation) {
  auto filename = std::unique_ptr<gchar, decltype(&g_free)>(
      g_filename_from_utf8(path.c_str(), -1, nullptr, nullptr, nullptr), g_free);
  if (filename == nullptr) throw std::runtime_error(_("Select an absolute local icon file path"));
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(filename.get()),
                                    g_object_unref);
  auto info_task = cardio::gio::submit<GFileInfo *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_query_info_async(file.get(),
            G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_query_info_finish(file.get(), result, error);
      }, cancellation);
  auto info = std::shared_ptr<GFileInfo>(co_await info_task, g_object_unref);
  validate_image_file_info(info.get());

  auto open_task = cardio::gio::submit<GFileInputStream *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_read_async(file.get(), G_PRIORITY_DEFAULT, signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_read_finish(file.get(), result, error);
      }, cancellation);
  auto stream = std::shared_ptr<GFileInputStream>(co_await open_task,
                                                   g_object_unref);
  std::shared_ptr<GdkPixbuf> decoded;
  std::exception_ptr failure;
  std::shared_ptr<GdkPixbufLoader> loader;
  bool loader_closed = false;
  IconDecodeState state;
  try {
    // Recheck the opened object, since the path can change after query_info.
    auto opened_info_task = cardio::gio::submit<GFileInfo *>(
        [stream](GCancellable *signal, GAsyncReadyCallback callback,
                 gpointer data) {
          g_file_input_stream_query_info_async(stream.get(),
              G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
              G_PRIORITY_DEFAULT, signal, callback, data);
        },
        [stream](GObject *, GAsyncResult *result, GError **error) {
          return g_file_input_stream_query_info_finish(stream.get(), result,
                                                       error);
        }, cancellation);
    auto opened_info = std::shared_ptr<GFileInfo>(co_await opened_info_task,
                                                  g_object_unref);
    validate_image_file_info(opened_info.get());
    std::size_t received = 0;
    for (;;) {
      auto chunk_task = cardio::gio::read_bytes(G_INPUT_STREAM(stream.get()),
                                                 64 * 1024, cancellation);
      auto bytes = std::shared_ptr<GBytes>(co_await chunk_task, g_bytes_unref);
      gsize length = 0;
      const auto *data = static_cast<const guint8 *>(g_bytes_get_data(bytes.get(), &length));
      if (length == 0) {
        if (loader == nullptr) throw std::runtime_error(_("Select a PNG or JPEG icon image"));
        GError *error = nullptr;
        const gboolean closed = gdk_pixbuf_loader_close(loader.get(), &error);
        loader_closed = true;
        g_clear_error(&error);
        if (!closed || state.oversized)
          throw std::runtime_error(_("Icon image dimensions are invalid"));
        decoded = square_image(gdk_pixbuf_loader_get_pixbuf(loader.get()));
        if (decoded == nullptr)
          throw std::runtime_error(_("Icon image could not be decoded"));
        break;
      }
      if (length > image_limit - received)
        throw std::runtime_error(_("Icon image exceeds the 64 MiB limit"));
      received += length;
      if (loader == nullptr) {
        const char *type = image_type(data, length);
        if (type == nullptr) throw std::runtime_error(_("Select a PNG or JPEG icon image"));
        GError *error = nullptr;
        loader.reset(gdk_pixbuf_loader_new_with_type(type, &error), g_object_unref);
        g_clear_error(&error);
        if (loader == nullptr)
          throw std::runtime_error(_("Icon image could not be decoded"));
        g_signal_connect(loader.get(), "size-prepared",
                         G_CALLBACK(on_size_prepared), &state);
      }
      GError *error = nullptr;
      const gboolean accepted = gdk_pixbuf_loader_write(loader.get(), data, length, &error);
      g_clear_error(&error);
      if (state.oversized)
        throw std::runtime_error(_("Icon image dimensions are invalid"));
      if (!accepted)
        throw std::runtime_error(_("Icon image could not be decoded"));
    }
  } catch (...) {
    failure = std::current_exception();
  }
  if (loader != nullptr && !loader_closed) {
    gdk_pixbuf_loader_close(loader.get(), nullptr);
  }
  try {
    auto close_task = cardio::gio::close(G_INPUT_STREAM(stream.get()));
    co_await close_task;
  } catch (...) {
    if (failure == nullptr) failure = std::current_exception();
  }
  if (failure != nullptr) std::rethrow_exception(failure);
  co_return decoded;
}

cardio::promise<ConnectionIconLoadResult>
load_connection_icon_async(std::string specification,
                           cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (specification.empty() || specification == "default")
    co_return {default_connection_icon(), false, {}};
  if (specification.starts_with("noto:") ||
      specification.starts_with("material:") ||
      specification.starts_with("glyphs-poly:")) {
    auto image = connection_icon_preset_thumbnail(specification, icon_size);
    if (image != nullptr) co_return {std::move(image), false, {}};
    co_return fallback_icon(_("Icon preset is unavailable"));
  }
  if (!specification.starts_with("file:"))
    co_return fallback_icon(_("Icon specification is not supported"));
  auto path = specification.substr(5);
  if (path.empty() || path.find('\0') != std::string::npos ||
      !g_utf8_validate(path.c_str(), -1, nullptr) ||
      !g_path_is_absolute(path.c_str()))
    co_return fallback_icon(_("Select an absolute local icon file path"));
  try {
    auto image = co_await read_icon_file_async(std::move(path), cancellation);
    co_return {std::move(image), false, {}};
  } catch (const cardio::canceled_exception &) {
    throw;
  } catch (const cardio::gio::gio_error &) {
    co_return fallback_icon(_("Icon file could not be read"));
  } catch (const std::exception &exception) {
    co_return fallback_icon(exception.what());
  }
}

} // namespace elder_terms
