// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-icon.h>
#include <elder-terms/settings.h>

#include <cardio.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace elder_terms_connection_icon_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void test_settings(const std::filesystem::path &directory) {
  auto store = elder_terms::create_default_settings(
      elder_terms::default_terminal_display_settings(12), "Connection");
  expect(elder_terms::general_connection_icon(store) == "default",
         "a new connection should use the default icon");
  const auto key = elder_terms::general_icon_setting_key();
  const auto connection = directory / "connection.ini";
  const auto global = directory / "global.ini";
  const std::string requested = "noto:future-emoji";
  expect(elder_terms::set_explicit_setting_value(&store, key, requested),
         "future preset IDs should remain selectable");
  expect(elder_terms::save_settings(store, connection).saved,
         "connection icon should save");
  auto loaded = elder_terms::load_settings(
      {.config_path = connection, .startup_config_path = std::nullopt,
       .global_config_path = global}, 12);
  expect(elder_terms::general_connection_icon(loaded.store) == requested,
         "unknown preset should survive reload");
  expect(elder_terms::save_global_settings(store, global).saved,
         "global defaults should save");
  std::ifstream input(global);
  const std::string global_text((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  expect(global_text.find("icon=") == std::string::npos,
         "connection icon must not be written into global defaults");
  {
    std::ofstream output(global);
    output << "[general]\nicon=file:/tmp/ignored.png\n";
  }
  loaded = elder_terms::load_settings(
      {.config_path = connection, .startup_config_path = std::nullopt,
       .global_config_path = global}, 12);
  expect(elder_terms::general_connection_icon(loaded.store) == requested,
         "connection icon should ignore the global value");
  expect(elder_terms::set_explicit_setting_value(
             &store, key, std::string("file:/missing/elsewhere.png")),
         "missing file paths should remain saved values");
}

static std::filesystem::path create_image(const std::filesystem::path &directory,
                                          const char *format) {
  const auto image = directory / (std::string("wide.") + format);
  auto pixbuf = std::shared_ptr<GdkPixbuf>(
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, std::string(format) == "png", 8, 40, 20),
      g_object_unref);
  expect(pixbuf != nullptr, "test image should allocate");
  gdk_pixbuf_fill(pixbuf.get(), 0xff000080U);
  GError *error = nullptr;
  const gboolean saved = gdk_pixbuf_save(pixbuf.get(), image.c_str(), format,
                                        &error, nullptr);
  if (error != nullptr) g_error_free(error);
  expect(saved, "test PNG should save");
  return image;
}

static std::filesystem::path create_oversized_dimensions(
    const std::filesystem::path &directory,
    const std::filesystem::path &image) {
  std::ifstream input(image, std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
  expect(bytes.size() > 33, "source PNG should contain an IHDR chunk");
  const auto write_u32 = [&bytes](std::size_t offset, unsigned int value) {
    for (int shift = 24; shift >= 0; shift -= 8)
      bytes[offset++] = static_cast<unsigned char>(value >> shift);
  };
  write_u32(16, 5000);
  write_u32(20, 5000);
  unsigned int crc = 0xffffffffU;
  for (std::size_t index = 12; index < 29; ++index) {
    crc ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  write_u32(29, ~crc);
  const auto path = directory / "oversized-dimensions.png";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  return path;
}

static void expect_square(const elder_terms::ConnectionIconLoadResult &result) {
  expect(result.image != nullptr, "result should have an image");
  expect(gdk_pixbuf_get_width(result.image.get()) == 256 &&
             gdk_pixbuf_get_height(result.image.get()) == 256 &&
             gdk_pixbuf_get_has_alpha(result.image.get()),
         "icon should be square 256-pixel RGBA");
}

static cardio::promise<void> test_loading_async(
    const std::filesystem::path &image, const std::filesystem::path &jpeg,
    const std::filesystem::path &invalid, const std::filesystem::path &huge,
    const std::filesystem::path &oversized_dimensions,
    cardio::dispatcher_group_glib *group,
    std::exception_ptr *failure) {
  try {
    auto standard = co_await elder_terms::load_connection_icon_async(
        "default", {});
    expect_square(standard);
    expect(!standard.fallback && standard.error.empty(),
           "requested default should not be diagnosed as fallback");
    expect(elder_terms::default_connection_icon() != nullptr,
           "default image should be directly available");

    for (const char *specification : {
             "noto:emoji_u1f44d", "material:action/home", "material:white:action/home",
             "glyphs-poly:folder"}) {
      auto preset = co_await elder_terms::load_connection_icon_async(
          specification, {});
      expect_square(preset);
      expect(!preset.fallback && preset.error.empty(),
             "bundled preset should resolve without fallback");
    }

    auto loaded = co_await elder_terms::load_connection_icon_async(
        "file:" + image.string(), {});
    expect_square(loaded);
    expect(!loaded.fallback && loaded.error.empty(),
           "valid PNG should load without fallback");
    const auto *top = gdk_pixbuf_read_pixels(loaded.image.get());
    const auto stride = gdk_pixbuf_get_rowstride(loaded.image.get());
    expect(top[3] == 0 && top[3 * stride + 128 * 4 + 3] == 0,
           "wide image should have transparent vertical padding");
    const auto *middle = top + 128 * stride + 128 * 4;
    expect(middle[0] > 200 && middle[1] < 20 && middle[2] < 20 &&
               middle[3] > 100,
           "center should retain the PNG's translucent red pixel");
    expect((top + 128 * stride)[3] > 100 &&
               (top + 32 * stride + 128 * 4)[3] == 0,
           "small wide images should fill the icon width with vertical padding");

    auto saved_store = elder_terms::create_default_settings(
        elder_terms::default_terminal_display_settings(12), "Connection");
    const auto specification = "file:" + image.string();
    expect(elder_terms::set_explicit_setting_value(
               &saved_store, elder_terms::general_icon_setting_key(),
               specification),
           "file icon should be accepted as a connection setting");
    const auto config = image.parent_path() / "saved-image.ini";
    expect(elder_terms::save_settings(saved_store, config).saved,
           "file icon should save");
    const auto restored = elder_terms::load_settings(
        {.config_path = config, .startup_config_path = std::nullopt,
         .global_config_path = image.parent_path() / "unused-global.ini"}, 12);
    expect(elder_terms::general_connection_icon(restored.store) == specification,
           "saved file icon should survive restart");
    auto reopened = co_await elder_terms::load_connection_icon_async(
        elder_terms::general_connection_icon(restored.store), {});
    expect_square(reopened);
    expect(!reopened.fallback, "restored file icon should decode");
    const auto *reopened_middle = gdk_pixbuf_read_pixels(reopened.image.get()) +
        128 * gdk_pixbuf_get_rowstride(reopened.image.get()) + 128 * 4;
    expect(reopened_middle[0] > 200 && reopened_middle[1] < 20 &&
               reopened_middle[2] < 20 && reopened_middle[3] > 100,
           "restored file icon should retain its red pixel");

    auto jpeg_result = co_await elder_terms::load_connection_icon_async(
        "file:" + jpeg.string(), {});
    expect_square(jpeg_result);
    expect(!jpeg_result.fallback && jpeg_result.error.empty(),
           "valid JPEG should load without fallback");
    const auto *jpeg_middle = gdk_pixbuf_read_pixels(jpeg_result.image.get()) +
        128 * gdk_pixbuf_get_rowstride(jpeg_result.image.get()) + 128 * 4;
    expect(jpeg_middle[0] > 200 && jpeg_middle[3] == 255,
           "JPEG content should retain its opaque red pixel");

    for (const char *request : {
             "file:/missing/elder-terms-icon.png", "noto:future-emoji",
             "material:future-icon", "glyphs-poly:unknown", "file:relative.png", "unknown:value"}) {
      auto fallback = co_await elder_terms::load_connection_icon_async(request, {});
      expect_square(fallback);
      expect(fallback.fallback && !fallback.error.empty(),
             "unavailable requests should diagnose fallback");
    }
    for (const auto &path : {invalid, huge}) {
      auto fallback = co_await elder_terms::load_connection_icon_async(
          "file:" + path.string(), {});
      expect_square(fallback);
      expect(fallback.fallback && !fallback.error.empty(),
             "invalid or oversized file should fall back with a reason");
    }
    auto oversized = co_await elder_terms::load_connection_icon_async(
        "file:" + oversized_dimensions.string(), {});
    expect(oversized.fallback &&
               oversized.error.find("dimensions") != std::string::npos,
           "declared decoded image size above 64 MiB should be rejected");

    cardio::cancellation_source canceled;
    canceled.cancel();
    bool threw = false;
    try {
      auto ignored = co_await elder_terms::load_connection_icon_async(
          "file:" + image.string(), canceled.get_cancellation());
      (void)ignored;
    } catch (const cardio::canceled_exception &) {
      threw = true;
    }
    expect(threw, "cancellation should propagate");
  } catch (...) {
    *failure = std::current_exception();
  }
  group->shutdown();
}

} // namespace elder_terms_connection_icon_test

int main() {
  try {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("elder-terms-connection-icon-" +
                            std::to_string(g_get_monotonic_time()));
    std::filesystem::create_directories(directory);
    const auto image = elder_terms_connection_icon_test::create_image(directory, "png");
    const auto jpeg = elder_terms_connection_icon_test::create_image(directory, "jpeg");
    const auto oversized_dimensions =
        elder_terms_connection_icon_test::create_oversized_dimensions(directory, image);
    const auto invalid = directory / "invalid.png";
    {
      std::ofstream output(invalid);
      output << "not an image";
    }
    const auto huge = directory / "huge.png";
    {
      std::ofstream output(huge, std::ios::binary);
      output.seekp(64 * 1024 * 1024);
      output.put('\0');
    }
    elder_terms_connection_icon_test::test_settings(directory);
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib dispatcher(group);
      auto task = elder_terms_connection_icon_test::test_loading_async(
          image, jpeg, invalid, huge, oversized_dimensions, &group, &failure);
      dispatcher.park();
    }
    std::filesystem::remove_all(directory);
    if (failure != nullptr) std::rethrow_exception(failure);
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
