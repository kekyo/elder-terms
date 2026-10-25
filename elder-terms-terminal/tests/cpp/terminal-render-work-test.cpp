// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-runtime.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

using namespace elder_terms;

static std::size_t frames = 0;

extern "C" boxrig_ui_status __real_boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *, boxrig_ui_component_frame_result *);

extern "C" boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_component_frame_result *result) {
  ++frames;
  return __real_boxrig_gtk3_terminal_widget_frame(widget, result);
}

static cardio::promise<void> run(TerminalRuntime *runtime,
    cardio::primitives::conditional *changed, std::exception_ptr *failure, bool opengl) {
  std::string incoming;
  std::string outgoing;
  cardio::primitives::conditional received;
  try {
    while (!terminal_runtime_is_settled(runtime)) co_await changed->wait();
    auto *initial_widget = terminal_runtime_widget(runtime);
    if (boxrig_gtk3_terminal_widget_get_accessibility_enabled(initial_widget) ||
        atk_text_get_character_count(ATK_TEXT(gtk_widget_get_accessible(GTK_WIDGET(initial_widget)))) != 0)
      throw std::runtime_error("terminal text accessibility is disabled by default");
    if (boxrig_gtk3_terminal_widget_set_accessibility_enabled(initial_widget, true) != BOXRIG_UI_OK)
      throw std::runtime_error("enable test text observations");
    std::string input = "\033[?25l\033[H";
    for (std::size_t row = 0; row < 20; ++row) input += std::string(70, 'a') + "\r\n";
    if (!terminal_runtime_feed(runtime, input)) throw std::runtime_error("feed terminal text");
    while (!terminal_runtime_is_settled(runtime)) co_await changed->wait();
    frames = 0;
    terminal_runtime_set_hyperlink_rules(runtime, default_hyperlink_action_rules(), {});
    while (!terminal_runtime_is_settled(runtime)) co_await changed->wait();
    g_print("Frames while recognizing unchanged text: %zu\n", frames);
    // Count expensive screen preparations instead of setting a machine-speed
    // threshold. Allow the initial request and one coalesced completion.
    if (frames > 2) throw std::runtime_error("recognizing text without links must not repeatedly prepare the same screen");
    if (!terminal_runtime_feed(runtime, "\033[H\033[2Khttps://example.test/"))
      throw std::runtime_error("replace a line with a link");
    while (!terminal_runtime_is_settled(runtime)) co_await changed->wait();
    auto *view = boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(runtime));
    std::size_t annotations = 0;
    if (boxrig_ui_view_query_text_annotation_visual_ranges(view, 0, nullptr, 0, &annotations) !=
        BOXRIG_UI_LIMIT_EXCEEDED || annotations != 1)
      throw std::runtime_error("recognition still publishes visible link annotations");
    auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(terminal_runtime_widget(runtime)));
    auto *text = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
    const bool retained = text && std::string(text).starts_with("https://example.test/") &&
        std::string(text).find(std::string(70, 'a')) != std::string::npos;
    g_free(text);
    if (!retained) throw std::runtime_error("accessible text follows actual changes after recognition");

    // Chrome can cause another window allocation without changing the terminal
    // grid. That notification must not prepare the same terminal image again.
    auto *window = gtk_widget_get_toplevel(GTK_WIDGET(terminal_runtime_widget(runtime)));
    GtkAllocation allocation{};
    gtk_widget_get_allocation(window, &allocation);
    frames = 0;
    g_signal_emit_by_name(window, "size-allocate", &allocation);
    while (!terminal_runtime_is_settled(runtime)) co_await changed->wait();
    g_print("Frames after an unchanged window allocation: %zu\n", frames);
    if (frames != 0) throw std::runtime_error("unchanged window allocations must not prepare another terminal image");

    TerminalTransportRequest request;
    request.open = [&](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
      co_return TerminalTransport{
          [&](std::span<unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<std::size_t> {
            while (incoming.empty()) co_await received.wait(stop);
            const auto count = std::min(bytes.size(), incoming.size());
            std::memcpy(bytes.data(), incoming.data(), count);
            incoming.erase(0, count);
            co_return count;
          },
          [&](std::span<const unsigned char> bytes, cardio::cancellation) -> cardio::promise<void> {
            outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            changed->trigger();
            co_return;
          },
          [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
          []() -> cardio::promise<void> { co_return; }, {}, {}, {}};
    };
    if (!terminal_runtime_connect_transport(runtime, std::move(request)))
      throw std::runtime_error("connect synchronized-output peer");
    while (terminal_runtime_connection_state(runtime).phase != TerminalConnectionPhase::connected ||
        !terminal_runtime_is_settled(runtime)) co_await changed->wait();
    auto *widget = terminal_runtime_widget(runtime);
    auto *terminal = boxrig_ui_view_terminal(boxrig_gtk3_terminal_widget_view(widget));
    // A native key creates outbound bytes inside the widget frame. Sending
    // those bytes must not prepare a second image while the peer is silent.
    frames = 0;
    for (const auto type : {GDK_KEY_PRESS, GDK_KEY_RELEASE}) {
      auto *event = gdk_event_new(type);
      event->key.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget))));
      event->key.keyval = GDK_KEY_Down;
      event->key.hardware_keycode = 116;
      auto *seat = gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(widget)));
      gdk_event_set_device(event, gdk_seat_get_keyboard(seat));
      gboolean handled = FALSE;
      g_signal_emit_by_name(widget, type == GDK_KEY_PRESS ? "key-press-event" : "key-release-event", event, &handled);
      gdk_event_free(event);
      if (!handled) throw std::runtime_error("terminal accepts native arrow input");
    }
    while (outgoing.empty() || !terminal_runtime_is_settled(runtime)) co_await changed->wait();
    if (outgoing != "\033[B") throw std::runtime_error("outgoing arrow still reaches the peer");
    g_print("Frames while transmitting without screen changes: %zu\n", frames);
    if (frames != 1) throw std::runtime_error("outgoing progress must not request another terminal image");
    // The runtime can settle before the queued OpenGL paint completes. Finish
    // that frame before using the presentation generation as the batch baseline.
    auto *frame_clock = gtk_widget_get_frame_clock(GTK_WIDGET(widget));
    if (!frame_clock) throw std::runtime_error("terminal has a frame clock");
    cardio::primitives::conditional painted;
    const auto paint_handler = g_signal_connect(frame_clock, "after-paint",
        G_CALLBACK(+[](GdkFrameClock *, gpointer context) {
          static_cast<cardio::primitives::conditional *>(context)->trigger();
        }), &painted);
    gtk_widget_queue_draw(GTK_WIDGET(widget));
    co_await painted.wait();
    g_signal_handler_disconnect(frame_clock, paint_handler);
    const auto generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
    incoming = "\033[?2026h\033[H\033[2KCOMPLETE BATCH";
    received.trigger();
    while (!incoming.empty() || !boxrig_terminal_synchronized_output(terminal)) co_await changed->wait();
    // A lower-priority ready source runs after all pending receive/frame work.
    // No clock deadline is needed to test completion of an explicitly ended batch.
    const auto flush = []() -> cardio::promise<void> {
      cardio::primitives::conditional ready;
      g_idle_add_full(G_PRIORITY_LOW, +[](gpointer context) -> gboolean {
        static_cast<cardio::primitives::conditional *>(context)->trigger();
        return G_SOURCE_REMOVE;
      }, &ready, nullptr);
      co_await ready.wait();
    };
    co_await flush();
    if (boxrig_gtk3_terminal_widget_presentation_generation(widget) != generation) {
      g_printerr("Incomplete batch: previous=%llu current=%llu synchronized=%d frames=%zu\n",
          static_cast<unsigned long long>(generation),
          static_cast<unsigned long long>(boxrig_gtk3_terminal_widget_presentation_generation(widget)),
          boxrig_terminal_synchronized_output(terminal), frames);
      throw std::runtime_error("an incomplete synchronized batch retains the previous screen");
    }
    incoming = "\033[?2026l";
    received.trigger();
    co_await flush();
    const bool presented = boxrig_gtk3_terminal_widget_presentation_generation(widget) > generation;
    if (!opengl && !presented)
      throw std::runtime_error("the batch end presents pending text without waiting for an animation deadline");
    while (!terminal_runtime_is_settled(runtime)) co_await changed->wait();
    if (opengl) {
      boxrig_gtk_render_state render{};
      boxrig_gtk3_terminal_widget_get_render_state(widget, &render);
      if (render.active_backend != BOXRIG_GTK_RENDER_BACKEND_OPENGL)
        throw std::runtime_error("OpenGL test needs a functioning renderer");
      // Keep the first image pending, as it would be before the native frame
      // clock paints. New input must still advance the component meanwhile.
      const auto hold = g_signal_connect(widget, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer) -> gboolean {
        return TRUE;
      }), nullptr);
      incoming = "\033[?2026h\033[H\033[2KFIRST UPDATE\033[?2026l";
      received.trigger();
      co_await flush();
      const auto before = frames;
      incoming = "\033[?2026h\033[H\033[2KLATEST UPDATE\033[?2026l";
      received.trigger();
      co_await flush();
      auto *latest = atk_text_get_text(ATK_TEXT(gtk_widget_get_accessible(GTK_WIDGET(widget))), 0, -1);
      const bool current = latest && std::string(latest).starts_with("LATEST UPDATE");
      g_free(latest);
      g_signal_handler_disconnect(widget, hold);
      gtk_widget_queue_draw(GTK_WIDGET(widget));
      g_print("Frames while new input arrives before paint: %zu\n", frames - before);
      if (!current) throw std::runtime_error("new input advances the pending frame before native paint");
    }
  } catch (...) {
    *failure = std::current_exception();
  }
  co_await terminal_runtime_disconnect(runtime);
  gtk_main_quit();
}

int main(int argc, char **argv) {
  const bool opengl = argc > 1 && std::string(argv[1]) == "opengl";
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), content);
  auto *runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content), GTK_BOX(content), elder_terms::TerminalAmbiguousWidth::narrow);
  if (!runtime) return 1;
  cardio::primitives::conditional changed;
  terminal_runtime_set_changed_callback(runtime, +[](void *context) {
    static_cast<cardio::primitives::conditional *>(context)->trigger();
  }, &changed);
  gtk_widget_show_all(window);
  std::exception_ptr failure;
  auto completion = run(runtime, &changed, &failure, opengl);
  gtk_main();
  destroy_terminal_runtime(runtime);
  gtk_widget_destroy(window);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  return 0;
}
