// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  cardio::primitives::conditional changed;
  cardio::primitives::conditional received;
  cardio::primitives::conditional write_ready;
  std::string incoming;
  std::string outgoing;
  std::string cooked;
  std::vector<std::vector<std::string>> commands;
  std::vector<std::string> errors;
  bool hold_write = false;
  bool write_pending = false;
  bool closed = false;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static std::string text(Fixture *fixture) {
  auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(terminal_runtime_widget(fixture->runtime)));
  auto *value = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  std::string result(value);
  g_free(value);
  return result;
}

static TerminalTransportRequest request(Fixture *fixture) {
  TerminalTransportRequest request;
  request.queue_capacity = 64;
  request.io_chunk_size = 16;
  request.open = [fixture](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    fixture->closed = false;
    co_return TerminalTransport{
      [fixture](std::span<unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<std::size_t> {
        while (fixture->incoming.empty()) co_await fixture->received.wait(stop);
        const auto count = std::min(bytes.size(), fixture->incoming.size());
        std::memcpy(bytes.data(), fixture->incoming.data(), count);
        fixture->incoming.erase(0, count);
        fixture->changed.trigger();
        co_return count;
      },
      [fixture](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
        fixture->write_pending = true;
        fixture->changed.trigger();
        try {
          if (fixture->hold_write) co_await fixture->write_ready.wait(stop);
          fixture->outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        } catch (...) {
          fixture->write_pending = false;
          fixture->changed.trigger();
          throw;
        }
        fixture->write_pending = false;
        fixture->changed.trigger();
      },
      [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
      [fixture]() -> cardio::promise<void> { fixture->closed = true; co_return; },
      {}, {}, {}, {},
    };
  };
  return request;
}

static void configure(Fixture *fixture, std::vector<MacroRule> rules) {
  terminal_runtime_set_macro_rules(fixture->runtime, std::move(rules), true,
      [fixture](std::string command, std::vector<std::string> args) {
        args.insert(args.begin(), std::move(command));
        fixture->commands.push_back(std::move(args));
      }, [fixture](std::string error) { fixture->errors.push_back(std::move(error)); });
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  configure(fixture, {
    {"ready", R"(READY (?<name>[\p{L}]+))", MacroSendAction{"${1}:${name}:$$\n"}},
    {"lower-priority", "READY", MacroSendAction{"wrong"}},
    {"control", R"(^\u{1b}\[31mERROR (?<code>[0-9]+)$)", MacroCommandAction{"tool-${1}", {"${0}", "$${code}"}}},
  });
  TerminalTextSettings settings;
  settings.return_code = TerminalReturnCode::crlf;
  terminal_runtime_request_input_settings(fixture->runtime, settings);
  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "connect macros transport");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->incoming = "READY 記者";
  fixture->received.trigger();
  while (text(fixture).find("READY 記者") == std::string::npos || !terminal_runtime_is_settled(fixture->runtime))
    co_await fixture->changed.wait();
  expect(fixture->outgoing == "記者:記者:$\r\n", "received macro reaches transport with applied newline policy before LF");
  fixture->incoming = "\r\n\x1b[31mERROR 42\r\n";
  fixture->received.trigger();
  while (text(fixture).find("ERROR 42") == std::string::npos || !terminal_runtime_is_settled(fixture->runtime))
    co_await fixture->changed.wait();
  expect(fixture->commands == std::vector<std::vector<std::string>>{{"tool-42", "\x1b[31mERROR 42", "${code}"}},
      "command receives control bytes and capture expansion");
  expect(fixture->cooked == "READY 記者\r\n\x1b[31mERROR 42\r\n", "each decoded byte is still reported once");

  configure(fixture, {{"modal", "^MODAL$", MacroSendAction{"still-replying\n"}}});
  terminal_runtime_request_input_enabled(fixture->runtime, false);
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  fixture->outgoing.clear();
  fixture->incoming = "MODAL\r\n";
  fixture->received.trigger();
  while (text(fixture).find("MODAL") == std::string::npos || !terminal_runtime_is_settled(fixture->runtime))
    co_await fixture->changed.wait();
  expect(fixture->outgoing == "still-replying\r\n",
      "automatic receive replies continue while a modal dialog inhibits keyboard input");
  terminal_runtime_request_input_enabled(fixture->runtime, true);

  const std::string payload = std::string(4095, 'x') + "日本語" + std::string(70000, 'y');
  configure(fixture, {{"large", "BIG", MacroSendAction{payload}},
      {"marker", "^MARK$", MacroCommandAction{"marker", {}}}});
  fixture->outgoing.clear();
  fixture->cooked.clear();
  fixture->hold_write = true;
  std::string incoming = "BIG\r\n";
  for (int line = 0; line < 4096; ++line) incoming += "quiet\n";
  incoming += "MARK\n";
  fixture->incoming = incoming;
  fixture->received.trigger();
  while (!fixture->write_pending) co_await fixture->changed.wait();
  expect(terminal_runtime_set_preferred_grid(fixture->runtime, 26, 86) == BOXRIG_UI_OK,
      "layout remains available while macro output is backpressured");
  boxrig_ui_layout_result layout{};
  do {
    co_await fixture->changed.wait();
    boxrig_gtk3_terminal_widget_get_layout(terminal_runtime_widget(fixture->runtime), &layout);
  } while (layout.rows != 26 || layout.columns != 86);
  expect(!fixture->incoming.empty() && fixture->cooked.size() <= 96,
      "blocked macro applies finite backpressure to further received notifications");
  fixture->hold_write = false;
  fixture->write_ready.trigger();
  while (fixture->commands.size() < 2 || fixture->outgoing.size() < payload.size() ||
      !terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  expect(fixture->outgoing == payload && fixture->cooked == incoming,
      "resumed UTF-8 chunks and receive notifications arrive once without loss");
  const auto state = terminal_runtime_connection_state(fixture->runtime);
  expect(state.receive_peak <= 64 && state.send_peak <= 64, "communication queues retain their configured bounds");

  fixture->hold_write = true;
  fixture->incoming = "BIG\n";
  fixture->received.trigger();
  while (!fixture->write_pending) co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(fixture->closed && !fixture->write_pending, "disconnect joins a blocked macro write");
  const auto generation = terminal_runtime_connection_state(fixture->runtime).generation;
  fixture->incoming.clear();
  fixture->outgoing.clear();
  fixture->hold_write = false;
  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "reconnect retains macro rules");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->incoming = "MARK\n";
  fixture->received.trigger();
  while (fixture->commands.size() < 3 || !terminal_runtime_is_settled(fixture->runtime))
    co_await fixture->changed.wait();
  expect(terminal_runtime_connection_state(fixture->runtime).generation > generation && fixture->outgoing.empty(),
      "old macro payload is never sent to a new connection generation");
  expect(fixture->errors.empty(), "valid runtime macros have no diagnostic");
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->runtime);
  gtk_main_quit();
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), content);
  Fixture fixture;
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content), GTK_BOX(content), elder_terms::TerminalAmbiguousWidth::narrow);
  if (!fixture.runtime) return 1;
  expect(boxrig_gtk3_terminal_widget_set_accessibility_enabled(
      terminal_runtime_widget(fixture.runtime), true) == BOXRIG_UI_OK, "enable test text observations");
  terminal_runtime_set_output_callback(fixture.runtime,
      [&fixture](std::span<const unsigned char>, std::span<const unsigned char> cooked) {
        fixture.cooked.append(reinterpret_cast<const char *>(cooked.data()), cooked.size());
      });
  terminal_runtime_set_changed_callback(fixture.runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show_all(window);
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
