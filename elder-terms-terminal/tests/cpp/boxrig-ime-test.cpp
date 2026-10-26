// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  GtkWidget *entry = nullptr;
  cardio::primitives::conditional changed;
  cardio::primitives::conditional received;
  std::string incoming;
  std::string outgoing;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static boxrig_gtk_ime_state ime(Fixture *fixture) {
  boxrig_gtk_ime_state result{};
  expect(boxrig_gtk3_terminal_widget_get_ime_state(terminal_runtime_widget(fixture->runtime), &result) == BOXRIG_UI_OK,
      "read public IME diagnostics");
  return result;
}

static std::string preedit(Fixture *fixture) {
  boxrig_composition_view result{};
  expect(boxrig_composition_get_view(boxrig_gtk3_terminal_widget_composition(
      terminal_runtime_widget(fixture->runtime)), &result) == BOXRIG_COMPOSITION_OK, "read preedit snapshot");
  return result.text_length == 0 ? std::string{} : std::string(result.text, result.text_length);
}

static std::string body(Fixture *fixture) {
  auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(terminal_runtime_widget(fixture->runtime)));
  auto *value = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  const std::string result(value);
  g_free(value);
  return result;
}

static void focus(Fixture *fixture, bool active) {
  auto *widget = GTK_WIDGET(terminal_runtime_widget(fixture->runtime));
  auto *event = gdk_event_new(GDK_FOCUS_CHANGE);
  event->focus_change.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(widget)));
  event->focus_change.in = active;
  gboolean handled = FALSE;
  g_signal_emit_by_name(widget, active ? "focus-in-event" : "focus-out-event", event, &handled);
  gdk_event_free(event);
}

static void key(Fixture *fixture, guint symbol) {
  auto *widget = GTK_WIDGET(terminal_runtime_widget(fixture->runtime));
  // The unchanged upstream fcitx4 fixture checks this native event metadata.
  for (const auto type : {GDK_KEY_PRESS, GDK_KEY_RELEASE}) {
    auto *event = gdk_event_new(type);
    event->key.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(widget)));
    event->key.keyval = symbol;
    event->key.hardware_keycode = 38;
    event->key.time = 1234;
    auto *seat = gdk_display_get_default_seat(gtk_widget_get_display(widget));
    gdk_event_set_device(event, gdk_seat_get_keyboard(seat));
    gboolean handled = FALSE;
    g_signal_emit_by_name(widget, type == GDK_KEY_PRESS ? "key-press-event" : "key-release-event", event, &handled);
    gdk_event_free(event);
    expect(handled, "widget owns the native key");
  }
}

static boxrig_ui_input_phase input_phase(Fixture *fixture) {
  boxrig_ui_input_state result{};
  boxrig_gtk3_terminal_widget_get_input_state(terminal_runtime_widget(fixture->runtime), &result);
  return result.phase;
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  TerminalTransportRequest request;
  request.open = [fixture](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
      [fixture](std::span<unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<std::size_t> {
        while (fixture->incoming.empty()) co_await fixture->received.wait(stop);
        const auto count = std::min(bytes.size(), fixture->incoming.size());
        std::memcpy(bytes.data(), fixture->incoming.data(), count);
        fixture->incoming.erase(0, count);
        co_return count;
      },
      [fixture](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
        stop.throw_if_cancellation_requested();
        fixture->outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        fixture->incoming.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        fixture->received.trigger();
        fixture->changed.trigger();
        co_return;
      },
      [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
      []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_runtime_connect_transport(fixture->runtime, std::move(request)), "connect IME echo transport");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected ||
      input_phase(fixture) != BOXRIG_UI_INPUT_ENABLED) co_await fixture->changed.wait();
  focus(fixture, true);
  while (ime(fixture).active_route != BOXRIG_GTK_IME_ROUTE_FCITX4 ||
      ime(fixture).direct.pending != 0 || ime(fixture).direct.contexts != 1) co_await fixture->changed.wait();
  expect(ime(fixture).requested_policy == BOXRIG_GTK_IME_AUTO, "runtime retains AUTO policy");
  expect(terminal_runtime_feed(fixture->runtime, "\033[HSELECT"), "prepare selection alongside composition");
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(fixture->runtime));
  boxrig_ui_hit_test_result hit{};
  expect(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK,
      "hit selectable text beside IME");
  expect(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_WORD, &hit) == BOXRIG_UI_OK,
      "select text before starting composition");
  key(fixture, GDK_KEY_a);
  while (preedit(fixture) != "きしゃ") co_await fixture->changed.wait();
  expect(fixture->outgoing.empty() && body(fixture).find("きしゃ") == std::string::npos,
      "composition is an overlay and has not reached the peer or terminal text");
  TerminalTextSettings settings;
  settings.cursor_key_mode = TerminalCursorKeyMode::trs80;
  terminal_runtime_request_input_settings(fixture->runtime, settings);
  co_await fixture->changed.wait();
  expect(terminal_runtime_input_settings_pending(fixture->runtime), "active composition retains the previous input policy");
  key(fixture, GDK_KEY_Escape);
  while (!preedit(fixture).empty() || terminal_runtime_input_settings_pending(fixture->runtime))
    co_await fixture->changed.wait();
  boxrig_ui_selection selection{};
  expect(boxrig_ui_view_get_selection(view, &selection) && fixture->outgoing.empty(),
      "the first Escape cancels fcitx4 composition while preserving selection");
  key(fixture, GDK_KEY_Escape);
  while (boxrig_ui_view_get_selection(view, &selection)) co_await fixture->changed.wait();
  expect(fixture->outgoing.empty(), "the next Escape cancels selection without reaching the peer");
  key(fixture, GDK_KEY_a);
  while (preedit(fixture) != "きしゃ") co_await fixture->changed.wait();
  key(fixture, GDK_KEY_b);
  while (fixture->outgoing != "汽車🙂" || body(fixture).find("汽車🙂") == std::string::npos)
    co_await fixture->changed.wait();

  key(fixture, GDK_KEY_a);
  while (preedit(fixture) != "きしゃ") co_await fixture->changed.wait();
  terminal_runtime_request_input_enabled(fixture->runtime, false);
  while (input_phase(fixture) != BOXRIG_UI_INPUT_DISABLED || !preedit(fixture).empty()) co_await fixture->changed.wait();
  terminal_runtime_request_input_enabled(fixture->runtime, true);
  while (input_phase(fixture) != BOXRIG_UI_INPUT_ENABLED || ime(fixture).direct.pending != 0)
    co_await fixture->changed.wait();
  key(fixture, GDK_KEY_b);
  while (fixture->outgoing != "汽車🙂汽車🙂" || body(fixture).find("汽車🙂汽車🙂") == std::string::npos)
    co_await fixture->changed.wait();

  const auto retained = body(fixture);
  gtk_widget_show(fixture->entry);
  gtk_widget_grab_focus(fixture->entry);
  focus(fixture, false);
  while (ime(fixture).direct.contexts != 0 || ime(fixture).direct.pending != 0) co_await fixture->changed.wait();
  gtk_entry_set_text(GTK_ENTRY(fixture->entry), "overlay only");
  expect(body(fixture) == retained && fixture->outgoing == "汽車🙂汽車🙂", "overlay focus keeps terminal text and peer input separate");
  gtk_widget_hide(fixture->entry);
  gtk_widget_grab_focus(GTK_WIDGET(terminal_runtime_widget(fixture->runtime)));
  focus(fixture, true);
  while (ime(fixture).direct.contexts != 1 || ime(fixture).direct.pending != 0) co_await fixture->changed.wait();
  key(fixture, GDK_KEY_a);
  while (preedit(fixture) != "きしゃ") co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(fixture->outgoing == "汽車🙂汽車🙂", "disconnect discards retained preedit without committing it");
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->runtime);
  gtk_main_quit();
}

int main() {
  gtk_init(nullptr, nullptr);
  g_object_set(gtk_settings_get_default(), "gtk-im-module", "gtk-im-context-simple", nullptr);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *overlay = gtk_overlay_new();
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), overlay);
  gtk_container_add(GTK_CONTAINER(overlay), content);
  Fixture fixture;
  fixture.entry = gtk_entry_new();
  gtk_widget_set_no_show_all(fixture.entry, TRUE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), fixture.entry);
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content), GTK_BOX(content), elder_terms::TerminalAmbiguousWidth::narrow);
  if (!fixture.runtime) return 1;
  expect(boxrig_gtk3_terminal_widget_set_key_router(terminal_runtime_widget(fixture.runtime),
      +[](const boxrig_key_event *event, void *context) {
        return terminal_runtime_route_selection_key(static_cast<TerminalRuntime *>(context), *event);
      }, fixture.runtime) == BOXRIG_UI_OK, "install application selection routing");
  expect(boxrig_gtk3_terminal_widget_set_accessibility_enabled(
      terminal_runtime_widget(fixture.runtime), true) == BOXRIG_UI_OK, "enable test text observations");
  terminal_runtime_set_changed_callback(fixture.runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show_all(window);
  gtk_widget_grab_focus(GTK_WIDGET(terminal_runtime_widget(fixture.runtime)));
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(window);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  return 0;
}
