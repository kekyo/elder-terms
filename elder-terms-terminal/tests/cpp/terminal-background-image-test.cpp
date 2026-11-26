// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-background-image.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>

int main() {
  using namespace elder_terms;
  auto *directory = g_dir_make_tmp("elder-background-image-XXXXXX", nullptr);
  assert(directory);
  const auto path = std::string(directory) + "/image.png";
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 4, 2);
  gdk_pixbuf_fill(pixbuf, 0x12345680);
  assert(gdk_pixbuf_save(pixbuf, path.c_str(), "png", nullptr, nullptr));
  auto *loop = g_main_loop_new(nullptr, FALSE);
  TerminalBackgroundImagePtr loaded;
  std::string failure;
  const auto run = [&](const std::string &file, GCancellable *cancellation) {
    bool done = false;
    load_terminal_background_image(file, cancellation, [&](auto image, auto error) {
      loaded = std::move(image);
      failure = std::move(error);
      done = true;
      g_main_loop_quit(loop);
    });
    if (!done) g_main_loop_run(loop);
  };
  run(path, nullptr);
  assert(failure.empty() && loaded);
  assert(loaded->width == 4 && loaded->height == 2 && loaded->rgba.size() == 32);
  assert(loaded->rgba[0] == 0x12 && loaded->rgba[1] == 0x34 && loaded->rgba[2] == 0x56 && loaded->rgba[3] == 0x80);
  const auto jpeg = std::string(directory) + "/image.jpg";
  assert(gdk_pixbuf_save(pixbuf, jpeg.c_str(), "jpeg", nullptr, "quality", "100", nullptr));
  run(jpeg, nullptr);
  assert(failure.empty() && loaded && loaded->rgba[3] == 255);
  const auto rgb = std::string(directory) + "/padded.png";
  auto *padded = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 3, 2);
  assert(padded && gdk_pixbuf_get_rowstride(padded) > 9);
  gdk_pixbuf_fill(padded, 0x112233ff);
  auto *last = gdk_pixbuf_get_pixels(padded) + gdk_pixbuf_get_rowstride(padded) + 6;
  last[0] = 0x44;
  assert(gdk_pixbuf_save(padded, rgb.c_str(), "png", nullptr, nullptr));
  g_object_unref(padded);
  run(rgb, nullptr);
  assert(failure.empty() && loaded && loaded->rgba.size() == 24);
  for (std::size_t offset = 0; offset < loaded->rgba.size(); offset += 4) {
    assert(loaded->rgba[offset] == (offset == 20 ? 0x44 : 0x11));
    assert(loaded->rgba[offset + 1] == 0x22 && loaded->rgba[offset + 2] == 0x33 && loaded->rgba[offset + 3] == 255);
  }
  run("", nullptr);
  assert(!loaded && failure.empty());
  for (const auto &invalid : {std::string("image.png"), std::string("https://example.com/image.png"), path + ".missing", std::string(directory)}) {
    run(invalid, nullptr);
    assert(!loaded && !failure.empty());
  }
  const auto corrupt = std::string(directory) + "/corrupt.png";
  assert(g_file_set_contents(corrupt.c_str(), "invalid", -1, nullptr));
  run(corrupt, nullptr);
  assert(!loaded && !failure.empty());
  for (const auto &dimensions : {std::pair{16385, 1}, std::pair{4097, 4096}}) {
    auto *large = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, dimensions.first, dimensions.second);
    assert(large);
    gdk_pixbuf_fill(large, 0x000000ff);
    assert(gdk_pixbuf_save(large, corrupt.c_str(), "png", nullptr, nullptr));
    g_object_unref(large);
    run(corrupt, nullptr);
    assert(!loaded && failure.find("16384") != std::string::npos);
  }
  const auto oversized = std::string(directory) + "/oversized.png";
  std::filesystem::copy_file(path, oversized);
  std::filesystem::resize_file(oversized, 64 * 1024 * 1024 + 1);
  run(oversized, nullptr);
  assert(!loaded && !failure.empty());
  auto *cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  run(path, cancel);
  assert(!loaded && !failure.empty());
  g_cancellable_reset(cancel);
  bool cancelled = false;
  load_terminal_background_image(path, cancel, [&](auto image, auto error) {
    assert(!image && !error.empty());
    cancelled = true;
    g_main_loop_quit(loop);
  });
  g_cancellable_cancel(cancel);
  if (!cancelled) g_main_loop_run(loop);
  g_object_unref(cancel);
  g_object_unref(pixbuf);
  g_main_loop_unref(loop);
  std::filesystem::remove_all(directory);
  g_free(directory);
  std::cout << "PASS PNG/JPEG/alpha, clear, malformed, missing, nonlocal, oversized and cancelled images\n";
}
