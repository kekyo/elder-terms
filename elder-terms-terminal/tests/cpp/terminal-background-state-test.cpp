// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-background.h"
#include "../../src/terminal-runtime.h"
#include <cassert>
#include <iostream>

int main(int argc, char **argv) {
  using namespace elder_terms;
  boxrig_ui_background_effect_state effect{BOXRIG_UI_BACKGROUND_EFFECT_PREFER, true, true,
      BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED, 0};
  assert(format_terminal_background_status(TerminalBackgroundMode::blur, true, effect) == "Frosted glass requested");
  effect.accepted = false;
  effect.reason = BOXRIG_UI_BACKGROUND_EFFECT_PENDING;
  assert(format_terminal_background_status(TerminalBackgroundMode::blur, true, effect) == "Transparent background: checking frosted glass support");
  effect.reason = BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
  assert(format_terminal_background_status(TerminalBackgroundMode::blur, true, effect) == "Transparent background: frosted glass request failed");
  effect.reason = BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
  assert(format_terminal_background_status(TerminalBackgroundMode::blur, true, effect) == "Transparent background: frosted glass is unavailable");
  assert(format_terminal_background_status(TerminalBackgroundMode::blur, false, effect) == "Opaque background: desktop transparency is unavailable");
  assert(format_terminal_background_status(TerminalBackgroundMode::transparent, true, effect) == "Transparent background");
  assert(format_terminal_background_status(TerminalBackgroundMode::opaque, true, effect) == "Opaque background");
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  MainWindow window{};
  window.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  prepare_terminal_background_window(window.window);
  window.root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window.window), window.root_box);
  window.runtime = create_terminal_runtime(GTK_WINDOW(window.window), GTK_BOX(window.root_box), GTK_BOX(window.root_box), elder_terms::TerminalAmbiguousWidth::narrow);
  assert(window.runtime);
  std::vector<std::function<void(TerminalBackgroundImagePtr, std::string)>> requests;
  auto *background = create_terminal_background(&window, [&](std::string, GCancellable *, auto complete) {
    requests.push_back(std::move(complete));
  });
  auto store = create_default_settings(default_terminal_display_settings(12), "Background");
  set_setting_value(&store, terminal_background_opacity_setting_key(), gint64{0});
  set_setting_value(&store, terminal_background_image_setting_key(), std::string("/image.png"));
  auto image = std::make_shared<TerminalBackgroundImage>(TerminalBackgroundImage{"/image.png", 2, 1,
      {255, 0, 0, 128, 0, 255, 0, 255}});
  assert(apply_terminal_background(background, store, image));
  auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(window.runtime));
  auto native = boxrig_ui_view_get_background_image(view);
  assert(native.width == 2 && native.height == 1 && native.pixels[0] == 0x80800000 && native.pixels[1] == 0xff00ff00);
  bool prepared = false;
  prepare_terminal_background_image(background, store, [] { return true; }, [&](auto result, auto error) {
    assert(result == image && error.empty()); prepared = true;
  });
  assert(prepared && requests.empty()); // Layout/opacity edits reuse pixels even if the original file disappears.
  set_setting_value(&store, terminal_background_image_opacity_setting_key(), gint64{50});
  set_setting_value(&store, terminal_background_image_fit_setting_key(), std::string("contain"));
  assert(apply_terminal_background(background, store, image));
  native = boxrig_ui_view_get_background_image(view);
  assert(native.fit == BOXRIG_UI_BACKGROUND_CONTAIN && native.pixels[0] == 0x40400000);
  assert(boxrig_ui_view_set_background_image(view, nullptr) == BOXRIG_UI_OK);
  attach_terminal_background(background);
  native = boxrig_ui_view_get_background_image(view);
  assert(native.width == 2 && native.pixels[0] == 0x40400000);

  set_setting_value(&store, terminal_background_image_setting_key(), std::string("/replacement.png"));
  int completed = 0;
  const auto completion = [&](auto, auto) { ++completed; };
  prepare_terminal_background_image(background, store, [] { return true; }, completion);
  cancel_terminal_background_image(background);
  requests[0](image, {});
  assert(completed == 0 && terminal_background_image(background) == image);
  bool current = true;
  prepare_terminal_background_image(background, store, [&] { return current; }, completion);
  current = false;
  requests[1](image, {});
  assert(completed == 0);
  prepare_terminal_background_image(background, store, [] { return true; }, completion);
  prepare_terminal_background_image(background, store, [] { return true; }, completion);
  requests[2](image, {});
  requests[3](nullptr, "Broken image");
  assert(completed == 1 && terminal_background_image(background) == image);
  assert(terminal_background_status(background).find("Broken image") != std::string::npos);
  prepare_terminal_background_image(background, store, [] { return true; }, completion);
  destroy_terminal_background(background);
  requests[4](image, {});
  assert(completed == 1);
  destroy_terminal_runtime(window.runtime);
  gtk_widget_destroy(window.window);
  std::cout << "PASS alpha, placement, cached pixels, reattachment and stale image requests\n";
}
