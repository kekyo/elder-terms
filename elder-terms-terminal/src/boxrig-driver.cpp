// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-runtime.h"
#include "terminal-presentation.h"
#include "terminal-transports.h"
#include "inline-prompt.h"
#include "terminal-log.h"
#include "sftp/sftp-client.h"

#include <gestament/gtk.h>
#include <glib-unix.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <optional>
#include <filesystem>
#include <array>

struct Driver {
  elder_terms::TerminalRuntime *runtime = nullptr;
  GtkWidget *status = nullptr;
  GtkWidget *connection_status = nullptr;
  GtkWidget *break_button = nullptr;
  GtkWidget *sftp_button = nullptr;
  std::string sftp_path;
  std::shared_ptr<elder_terms::RemoteFileClient> sftp;
  std::optional<cardio::promise<void>> sftp_task;
  cardio::cancellation_source sftp_stop;
  bool sftp_active = false;
  elder_terms::TerminalLocalCommand command;
  std::optional<elder_terms::TerminalTransportRequest> transport;
  std::optional<elder_terms::SerialConnectionSettings> serial;
  std::shared_ptr<elder_terms::InlinePromptController> prompt;
  std::unique_ptr<elder_terms::TerminalLogState, decltype(&elder_terms::destroy_terminal_log)> log{nullptr, elder_terms::destroy_terminal_log};
  std::uint64_t log_generation = 0;
  elder_terms::TerminalTextSettings input_settings;
  elder_terms::TerminalTextSendSource text_source = elder_terms::TerminalTextSendBufferSource{"Text send from boxrig\n"};
  std::uint64_t text_rate = 1024;
  elder_terms::TerminalTransferRequest transfer;
  bool connection_mode = false;
  bool zmodem_auto = false;
  bool links = false;
  bool context_loss_control = false;
  gulong context_loss_hook = 0;
  std::vector<elder_terms::MacroRule> macros;
  bool closing = false;
  bool attachment_failed = false;
  std::string last_report;
  std::string last_connection_report;
  std::string last_encoding_report;
  std::uint64_t text_events = 0;
  std::uint64_t caret_events = 0;
  std::uint64_t selection_events = 0;
};

static void observe_log_connection(Driver *driver, const elder_terms::TerminalConnectionState &state) {
  if (state.phase == elder_terms::TerminalConnectionPhase::connected && state.generation != driver->log_generation) {
    elder_terms::set_terminal_log_connection_active(driver->log.get(), false);
    elder_terms::set_terminal_log_connection_active(driver->log.get(), true);
    driver->log_generation = state.generation;
  } else if (state.phase == elder_terms::TerminalConnectionPhase::closed) {
    elder_terms::set_terminal_log_connection_active(driver->log.get(), false);
  }
}

static void report_connection(Driver *driver) {
  if (!driver->connection_mode) return;
  const auto state = elder_terms::terminal_runtime_connection_state(driver->runtime);
  observe_log_connection(driver, state);
  const auto encoding_report = "encoding=" + state.encoding + " pending=" +
      (elder_terms::terminal_runtime_encoding_pending(driver->runtime) ? "1" : "0");
  if (encoding_report != driver->last_encoding_report) {
    driver->last_encoding_report = encoding_report;
    std::printf("%s\n", encoding_report.c_str());
    std::fflush(stdout);
  }
  const char *phase = state.phase == elder_terms::TerminalConnectionPhase::starting ? "starting" :
      state.phase == elder_terms::TerminalConnectionPhase::connected ? "connected" :
      state.phase == elder_terms::TerminalConnectionPhase::stopping ? "stopping" :
      state.phase == elder_terms::TerminalConnectionPhase::closed ? "closed" : "idle";
  char report[384];
  std::snprintf(report, sizeof(report),
      "connection=%s generation=%llu spawn=%d stage=%d errno=%d pid=%d exit=%d signal=%d cancelled=%d reaped=%d error=%d rx_peak=%zu tx_peak=%zu carrier=%d",
      phase, static_cast<unsigned long long>(state.generation), state.spawn.state, state.spawn.stage,
      state.spawn.error_number, state.child, state.exit_code, state.signal_number,
      state.cancelled, state.reaped, state.operation_error, state.receive_peak, state.send_peak, state.carrier_connected);
  if (driver->last_connection_report != report) {
    driver->last_connection_report = report;
    char label[128];
    std::snprintf(label, sizeof(label), "%s: spawn stage %d, errno %d, exit %d", phase,
        state.spawn.stage, state.spawn.error_number, state.exit_code);
    if (std::string_view(gtk_label_get_text(GTK_LABEL(driver->connection_status))) != label)
      gtk_label_set_text(GTK_LABEL(driver->connection_status), label);
    std::printf("%s\n", report);
    std::fflush(stdout);
  }
}

static void report_state(void *context) {
  auto *driver = static_cast<Driver *>(context);
  report_connection(driver);
  if (driver->break_button) gtk_widget_set_sensitive(driver->break_button,
      elder_terms::terminal_runtime_can_send_break(driver->runtime));
  if (driver->sftp_button) gtk_widget_set_sensitive(driver->sftp_button,
      !driver->sftp_active && (driver->sftp || elder_terms::terminal_runtime_authenticated_ssh(driver->runtime)));
  auto *widget = elder_terms::terminal_runtime_widget(driver->runtime);
  boxrig_gtk_render_state render{};
  boxrig_font_effective_metrics metrics{};
  boxrig_ui_layout_result layout{};
  if (boxrig_gtk3_terminal_widget_get_render_state(widget, &render) != BOXRIG_UI_OK || render.pending ||
      boxrig_gtk3_terminal_widget_get_layout(widget, &layout) != BOXRIG_UI_OK ||
      boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) != BOXRIG_FONT_CATALOG_OK) return;
  char text[256];
  const char *backend = render.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL ? "opengl" :
      render.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? "cairo" : "none";
  std::snprintf(text, sizeof(text), "backend=%s grid=%zux%zu font=%.2f reason=%d",
                backend, layout.columns, layout.rows, metrics.requested_size_dip, render.reason);
  // AT-SPI identifies this widget as a terminal. Also publish its actual
  // AtkText snapshot for automation clients without a terminal text adapter.
  auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(widget));
  auto *visible_text = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  auto *encoded = g_base64_encode(reinterpret_cast<const guchar *>(visible_text),
      std::char_traits<char>::length(visible_text));
  boxrig_ui_input_state input{};
  boxrig_gtk3_terminal_widget_get_input_state(widget, &input);
  boxrig_gtk_ime_state ime{};
  boxrig_gtk3_terminal_widget_get_ime_state(widget, &ime);
  boxrig_composition_view composition{};
  boxrig_composition_get_view(boxrig_gtk3_terminal_widget_composition(widget), &composition);
  auto *preedit = g_base64_encode(reinterpret_cast<const guchar *>(composition.text), composition.text_length);
  boxrig_ui_size_command_state size_state{};
  boxrig_gtk3_terminal_widget_get_size_command_state(widget, &size_state);
  const std::string report = std::string(text) + " cell=" + std::to_string(metrics.cell_width) +
      "x" + std::to_string(metrics.cell_height) + " selected=" + std::to_string(atk_text_get_n_selections(ATK_TEXT(accessible))) +
      " ready=" + (elder_terms::terminal_runtime_is_settled(driver->runtime) ? "1" : "0") +
      " input=" + std::to_string(input.phase) +
      " input_pending=" + (elder_terms::terminal_runtime_input_settings_pending(driver->runtime) ? "1" : "0") +
      " input_generation=" + std::to_string(input.generation) +
      " size_command=" + std::to_string(size_state) +
      " link_hover=" + (elder_terms::terminal_runtime_has_link_hover(driver->runtime) ? "1" : "0") +
      " ime_policy=" + std::to_string(ime.requested_policy) + " ime_route=" + std::to_string(ime.active_route) +
      " ime_reason=" + std::to_string(ime.reason) + " ime_state=" + std::to_string(ime.direct.state) +
      " ime_pending=" + std::to_string(ime.direct.pending) + " ime_contexts=" + std::to_string(ime.direct.contexts) +
      " ime_errors=" + std::to_string(ime.direct.errors) +
      " preedit_active=" + std::to_string(composition.active) +
      " commit_pending=" + std::to_string(composition.commit_pending) + " preedit=" + preedit +
      " caret=" + std::to_string(atk_text_get_caret_offset(ATK_TEXT(accessible))) +
      " text_events=" + std::to_string(driver->text_events) + " caret_events=" + std::to_string(driver->caret_events) +
      " selection_events=" + std::to_string(driver->selection_events) +
      " text=" + encoded;
  g_free(preedit);
  g_free(encoded);
  g_free(visible_text);
  if (report != driver->last_report) {
    driver->last_report = report;
    if (std::string_view(gtk_label_get_text(GTK_LABEL(driver->status))) != text)
      gtk_label_set_text(GTK_LABEL(driver->status), text);
    std::printf("%s\n", report.c_str());
    std::fflush(stdout);
  }
}

static void zoom_in(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  elder_terms::terminal_runtime_visual_zoom(driver->runtime, BOXRIG_FONT_ZOOM_INCREASE);
}

static void copy_selection(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  const boxrig_ui_view_action action{BOXRIG_UI_VIEW_ACTION_COPY, BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD};
  const auto status = boxrig_gtk3_terminal_widget_apply_view_action(elder_terms::terminal_runtime_widget(driver->runtime), &action);
  if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE)
    g_warning("boxrig copy failed: %d", status);
}

static gboolean lose_current_gl_context(GSignalInvocationHint *, guint, const GValue *values, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  auto *area = GTK_WIDGET(g_value_get_object(values));
  if (!gtk_widget_is_ancestor(area, GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime))))
    return TRUE;
  // GTK has made this area's context current before emitting render. Remove
  // it once so the normal preferred-renderer path observes a lost context.
  gdk_gl_context_clear_current();
  driver->context_loss_hook = 0;
  return FALSE;
}

static void lose_gl_context(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  if (!driver->context_loss_hook)
    driver->context_loss_hook = g_signal_add_emission_hook(g_signal_lookup("render", GTK_TYPE_GL_AREA),
        0, lose_current_gl_context, driver, nullptr);
  elder_terms::terminal_runtime_feed(driver->runtime, "\033[?25l");
}

static void paste_clipboard(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  const boxrig_ui_view_action action{BOXRIG_UI_VIEW_ACTION_PASTE, BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD};
  const auto status = boxrig_gtk3_terminal_widget_apply_view_action(elder_terms::terminal_runtime_widget(driver->runtime), &action);
  if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE)
    g_warning("boxrig paste failed: %d", status);
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static cardio::promise<void> close_driver(Driver *driver) {
  if (driver->closing) co_return;
  driver->closing = true;
  driver->sftp_stop.cancel();
  if (driver->sftp_task) co_await *driver->sftp_task;
  driver->sftp.reset();
  co_await elder_terms::terminal_runtime_disconnect(driver->runtime);
  co_await elder_terms::stop_terminal_log_async(driver->log.get());
  gtk_main_quit();
}

static gboolean close_window(GtkWidget *, GdkEvent *, gpointer context) {
  cardio::fire_and_forget(close_driver(static_cast<Driver *>(context)));
  return TRUE;
}

static gboolean stop_driver(gpointer context) {
  cardio::fire_and_forget(close_driver(static_cast<Driver *>(context)));
  return G_SOURCE_CONTINUE;
}

static void disconnect(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  cardio::fire_and_forget(elder_terms::terminal_runtime_disconnect(driver->runtime));
}

static bool connect_driver(Driver *driver) {
  return driver->transport ? elder_terms::terminal_runtime_connect_transport(driver->runtime, *driver->transport) :
      elder_terms::terminal_runtime_connect_local(driver->runtime, driver->command);
}

static void reconnect(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  connect_driver(driver);
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static void input_settings_changed(GtkButton *button, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  driver->input_settings = {};
  if (g_object_get_data(G_OBJECT(button), "trs80"))
    driver->input_settings.cursor_key_mode = elder_terms::TerminalCursorKeyMode::trs80;
  elder_terms::terminal_runtime_request_input_settings(driver->runtime, driver->input_settings);
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static boxrig_input_key_route route_key(const boxrig_key_event *event, void *context) {
  auto *driver = static_cast<Driver *>(context);
  const auto selection = elder_terms::terminal_runtime_route_selection_key(driver->runtime, *event);
  if (selection != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return selection;
  if (event->key != BOXRIG_KEY_F12 || event->modifiers != BOXRIG_MODIFIER_CONTROL)
    return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
  if (event->action == BOXRIG_KEY_PRESS) {
    driver->input_settings.cursor_key_mode = elder_terms::TerminalCursorKeyMode::trs80;
    elder_terms::terminal_runtime_request_input_settings(driver->runtime, driver->input_settings);
  }
  return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static void attach_driver_widget(Driver *driver, BoxrigGtk3TerminalWidget *widget) {
  driver->attachment_failed = boxrig_gtk3_terminal_widget_set_key_router(widget, route_key, driver) != BOXRIG_UI_OK ||
      elder_terms::apply_terminal_selection_style(boxrig_gtk3_terminal_widget_view(widget)) != BOXRIG_UI_OK;
  if (driver->attachment_failed) {
    std::fputs("Could not attach driver key routing\n", stderr);
    return;
  }
  gestament_gtk_assign_accessible_id(GTK_WIDGET(widget), "driver_terminal");
  auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(widget));
  const auto text_changed = +[](AtkText *, int, int, const char *, gpointer context) {
    ++static_cast<Driver *>(context)->text_events;
  };
  g_signal_connect(accessible, "text-insert", G_CALLBACK(text_changed), driver);
  g_signal_connect(accessible, "text-remove", G_CALLBACK(text_changed), driver);
  g_signal_connect(accessible, "text-caret-moved", G_CALLBACK(+[](AtkText *, int, gpointer context) {
    ++static_cast<Driver *>(context)->caret_events;
  }), driver);
  g_signal_connect(accessible, "text-selection-changed", G_CALLBACK(+[](AtkText *, gpointer context) {
    ++static_cast<Driver *>(context)->selection_events;
  }), driver);
}

static void input_enabled_changed(GtkButton *button, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  elder_terms::terminal_runtime_request_input_enabled(driver->runtime,
      g_object_get_data(G_OBJECT(button), "enabled") != nullptr);
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static void encoding_changed(GtkButton *button, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  const auto *encoding = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "encoding"));
  if (elder_terms::terminal_runtime_request_encoding(driver->runtime, encoding)) {
    driver->command.encoding = encoding;
    if (driver->transport) driver->transport->encoding = encoding;
  }
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static void baud_changed(GtkButton *button, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  auto settings = *driver->serial;
  settings.baudrate = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "baud"));
  try {
    driver->transport->configure_serial(settings);
    driver->serial = std::move(settings);
    std::printf("serial_baud=%lld generation=%llu\n", static_cast<long long>(driver->serial->baudrate),
        static_cast<unsigned long long>(elder_terms::terminal_runtime_connection_state(driver->runtime).generation));
    std::fflush(stdout);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Serial settings: %s\n", error.what());
  }
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static void send_text(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  elder_terms::TerminalTextSendRequest request;
  request.source = driver->text_source;
  request.bytes_per_second = driver->text_rate;
  request.active = [](bool active) { std::printf("text_send_active=%d\n", active); std::fflush(stdout); };
  request.finished = [](bool succeeded) { std::printf("text_send_finished=%d\n", succeeded); std::fflush(stdout); };
  request.status = [](const std::string &status) { std::fprintf(stderr, "Text send: %s\n", status.c_str()); };
  elder_terms::terminal_runtime_start_text_send(driver->runtime, std::move(request));
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static void cancel_transfer(GtkButton *, gpointer context) {
  elder_terms::terminal_runtime_cancel_transfer(static_cast<Driver *>(context)->runtime);
}

static void send_break(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  elder_terms::TerminalBreakRequest request;
  request.status = [](const std::string &status) { std::fprintf(stderr, "BREAK: %s\n", status.c_str()); };
  request.finished = [](bool succeeded) { std::printf("break_finished=%d\n", succeeded); std::fflush(stdout); };
  elder_terms::terminal_runtime_send_break(driver->runtime, std::move(request));
}

static cardio::promise<void> read_sftp(Driver *driver) {
  bool succeeded = false;
  const auto cancellation = driver->sftp_stop.get_cancellation();
  try {
    if (!driver->sftp) {
      auto transport = elder_terms::terminal_runtime_authenticated_ssh(driver->runtime);
      if (!transport) throw std::runtime_error("No authenticated SSH shell");
      driver->sftp = co_await elder_terms::open_sftp_client_async(std::move(transport), cancellation);
    }
    auto opening = driver->sftp->open_read_async(driver->sftp_path, cancellation);
    auto reader = std::move(co_await opening);
    std::array<std::byte, 4096> bytes{};
    while (const auto count = co_await reader->read_async(bytes, cancellation)) {
      auto *encoded = g_base64_encode(reinterpret_cast<const guchar *>(bytes.data()), count);
      std::printf("sftp_data=%s\n", encoded);
      g_free(encoded);
    }
    co_await reader->close_async(cancellation);
    succeeded = true;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    std::fprintf(stderr, "SFTP: %s\n", error.what());
  }
  driver->sftp_active = false;
  std::printf("sftp_finished=%d\n", succeeded);
  std::fflush(stdout);
  report_state(driver);
}

static void start_sftp(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  if (driver->closing || driver->sftp_active) return;
  driver->sftp_active = true;
  report_state(driver);
  driver->sftp_task.emplace(read_sftp(driver));
}

static elder_terms::TerminalTransferRequest transfer_request(Driver *driver) {
  auto request = driver->transfer;
  request.active = [](bool active) { std::printf("transfer_active=%d\n", active); std::fflush(stdout); };
  request.finished = [](bool succeeded) { std::printf("transfer_finished=%d\n", succeeded); std::fflush(stdout); };
  request.status = [](const std::string &status) { std::fprintf(stderr, "Transfer: %s\n", status.c_str()); };
  return request;
}

static void start_transfer(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  elder_terms::terminal_runtime_start_transfer(driver->runtime, transfer_request(driver));
  gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
}

static cardio::promise<elder_terms::SshUserPromptResponse> prompt_ssh(Driver *driver,
    const elder_terms::SshUserPrompt &prompt, cardio::cancellation cancellation) {
  elder_terms::InlinePromptRequest request{
      .title = prompt.title,
      .message = prompt.message,
      .monospace_message = prompt.monospace_message,
      .accept_label = prompt.kind == elder_terms::SshUserPromptKind::host_key ? "Accept" : "Connect",
      .cancel_label = "Cancel",
      .initial_text = prompt.initial_text,
      .input_required = prompt.input_required,
      .echo = prompt.echo,
      .cancel_visible = true,
      .accept_visible = prompt.accept_visible,
      .alternative_label = "Reset and Connect",
      .alternative_visible = prompt.host_key_reset_available,
  };
  const auto response = co_await elder_terms::prompt_inline_async(driver->prompt, std::move(request), cancellation);
  if (!driver->closing) gtk_widget_grab_focus(GTK_WIDGET(elder_terms::terminal_runtime_widget(driver->runtime)));
  co_return elder_terms::SshUserPromptResponse{response.accepted, response.text, response.alternative};
}

static void configure_links(Driver *driver) {
  elder_terms::terminal_runtime_set_hyperlink_rules(driver->runtime,
      driver->links ? elder_terms::default_hyperlink_action_rules() : std::vector<elder_terms::HyperlinkActionRule>{}, [](elder_terms::TerminalHyperlinkResolution resolved) {
        if (resolved.action) {
          std::string value = resolved.action->command;
          for (const auto &argument : resolved.action->arguments) {
            value.push_back('\0');
            value += argument;
          }
          auto *encoded = g_base64_encode(reinterpret_cast<const guchar *>(value.data()), value.size());
          std::printf("link=%s\n", encoded);
          g_free(encoded);
        } else {
          std::fprintf(stderr, "Link rejected: %s\n", resolved.error.c_str());
        }
        std::fflush(stdout);
      });
}

static void toggle_links(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  driver->links = !driver->links;
  configure_links(driver);
}

int main(int argc, char **argv) {
  // Arguments after -- belong to the child; GTK must not consume their flags.
  gtk_init(nullptr, nullptr);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  Driver driver;
  elder_terms::TerminalLogSettings log_settings;
  elder_terms::TelnetConnectionSettings telnet{};
  elder_terms::SshConnectionSettings ssh{};
  elder_terms::SerialConnectionSettings serial{
      .device = {}, .device_match_mode = elder_terms::SerialDeviceMatchMode::exact_path, .device_usb_serial = {},
      .baudrate = 9600, .bits = 8, .parity = elder_terms::SerialParity::none, .stop_bit = 1,
      .flow_control = elder_terms::SerialFlowControl::none, .carrier_detect = elder_terms::SerialCarrierDetect::ignore};
  elder_terms::AuthenticatedSshTransportOptions options;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--local") driver.connection_mode = true;
    else if (std::string_view(argv[index]) == "--links") driver.links = true;
    else if (std::string_view(argv[index]) == "--context-loss") driver.context_loss_control = true;
    else if (std::string_view(argv[index]) == "--macro-send" && index + 2 < argc) {
      const std::string pattern = argv[++index];
      driver.macros.push_back({"driver-" + std::to_string(driver.macros.size()), pattern,
          elder_terms::MacroSendAction{argv[++index]}});
    }
    else if (std::string_view(argv[index]) == "--macro-command" && index + 2 < argc) {
      const std::string pattern = argv[++index];
      driver.macros.push_back({"driver-" + std::to_string(driver.macros.size()), pattern,
          elder_terms::MacroCommandAction{argv[++index], {}}});
    }
    else if (std::string_view(argv[index]) == "--macro-arg" && index + 1 < argc) {
      if (driver.macros.empty() || !std::holds_alternative<elder_terms::MacroCommandAction>(driver.macros.back().action)) {
        std::fputs("--macro-arg requires a preceding --macro-command\n", stderr);
        return 2;
      }
      std::get<elder_terms::MacroCommandAction>(driver.macros.back().action).arguments.emplace_back(argv[++index]);
    }
    else if (std::string_view(argv[index]) == "--serial" && index + 1 < argc) {
      driver.connection_mode = true;
      serial.device = argv[++index];
    }
    else if (std::string_view(argv[index]) == "--baud" && index + 1 < argc)
      serial.baudrate = std::strtol(argv[++index], nullptr, 10);
    else if (std::string_view(argv[index]) == "--carrier" && index + 1 < argc) {
      const std::string_view value(argv[++index]);
      serial.carrier_detect = value == "cts" ? elder_terms::SerialCarrierDetect::cts :
          value == "dsr" ? elder_terms::SerialCarrierDetect::dsr :
          value == "cd" ? elder_terms::SerialCarrierDetect::cd : elder_terms::SerialCarrierDetect::ignore;
    }
    else if (std::string_view(argv[index]) == "--telnet" && index + 2 < argc) {
      driver.connection_mode = true;
      telnet.address = argv[++index];
      telnet.port = std::strtol(argv[++index], nullptr, 10);
      telnet.terminal_type = "xterm-256color";
    } else if (std::string_view(argv[index]) == "--ssh" && index + 3 < argc) {
      driver.connection_mode = true;
      ssh.endpoint.address = argv[++index];
      ssh.endpoint.port = std::strtol(argv[++index], nullptr, 10);
      ssh.endpoint.username = argv[++index];
      ssh.terminal_type = "xterm-256color";
    }
    else if (std::string_view(argv[index]) == "--identity" && index + 1 < argc)
      ssh.endpoint.identity_file = argv[++index];
    else if (std::string_view(argv[index]) == "--known-hosts" && index + 1 < argc)
      options.known_hosts_file = argv[++index];
    else if (std::string_view(argv[index]) == "--ssh-config" && index + 1 < argc)
      options.config_file = argv[++index];
    else if (std::string_view(argv[index]) == "--proxy" && index + 3 < argc) {
      options.proxy.enabled = true;
      options.proxy.endpoint.address = argv[++index];
      options.proxy.endpoint.port = std::strtol(argv[++index], nullptr, 10);
      options.proxy.endpoint.username = argv[++index];
    }
    else if (std::string_view(argv[index]) == "--proxy-identity" && index + 1 < argc)
      options.proxy.endpoint.identity_file = argv[++index];
    else if (std::string_view(argv[index]) == "--sftp-file" && index + 1 < argc)
      driver.sftp_path = argv[++index];
    else if (std::string_view(argv[index]) == "--trs80")
      driver.input_settings.cursor_key_mode = elder_terms::TerminalCursorKeyMode::trs80;
    else if (std::string_view(argv[index]) == "--encoding" && index + 1 < argc)
      driver.command.encoding = argv[++index];
    else if (std::string_view(argv[index]) == "--text" && index + 1 < argc)
      driver.text_source = elder_terms::TerminalTextSendBufferSource{argv[++index]};
    else if (std::string_view(argv[index]) == "--text-uri" && index + 1 < argc)
      driver.text_source = elder_terms::TerminalTextSendFileSource{argv[++index]};
    else if (std::string_view(argv[index]) == "--rate" && index + 1 < argc)
      driver.text_rate = std::strtoull(argv[++index], nullptr, 10);
    else if (std::string_view(argv[index]) == "--zmodem-auto") driver.zmodem_auto = true;
    else if (std::string_view(argv[index]) == "--transfer" && index + 2 < argc) {
      const std::string_view protocol = argv[++index];
      driver.transfer.protocol = protocol == "xmodem" ? elder_terms::TerminalTransferProtocol::xmodem :
          protocol == "ymodem" ? elder_terms::TerminalTransferProtocol::ymodem : elder_terms::TerminalTransferProtocol::zmodem;
      driver.transfer.direction = std::string_view(argv[++index]) == "receive" ?
          elder_terms::TerminalTransferDirection::receive : elder_terms::TerminalTransferDirection::send;
    }
    else if (std::string_view(argv[index]) == "--transfer-uri" && index + 1 < argc)
      driver.transfer.source_file_uris.emplace_back(argv[++index]);
    else if (std::string_view(argv[index]) == "--transfer-directory" && index + 1 < argc)
      driver.transfer.base_path = argv[++index];
    else if ((std::string_view(argv[index]) == "--log-raw" || std::string_view(argv[index]) == "--log-cooked") && index + 1 < argc) {
      log_settings.enabled = true;
      log_settings.mode = std::string_view(argv[index]) == "--log-raw" ? elder_terms::TerminalLogMode::raw : elder_terms::TerminalLogMode::cooked;
      const std::filesystem::path path(argv[++index]);
      log_settings.base_directory = path.parent_path().string();
      log_settings.file_name_format = path.filename().string();
    }
    else if (std::string_view(argv[index]) == "--return" && index + 1 < argc) {
      const std::string_view value = argv[++index];
      driver.input_settings.return_code = value == "cr" ? elder_terms::TerminalReturnCode::cr :
          value == "lf" ? elder_terms::TerminalReturnCode::lf :
          value == "crlf" ? elder_terms::TerminalReturnCode::crlf : elder_terms::TerminalReturnCode::automatic;
    } else if (std::string_view(argv[index]) == "--backspace" && index + 1 < argc) {
      const std::string_view value = argv[++index];
      driver.input_settings.backspace_code = value == "bs" ? elder_terms::TerminalBackspaceCode::bs :
          value == "del" ? elder_terms::TerminalBackspaceCode::del : elder_terms::TerminalBackspaceCode::automatic;
    }
    else if (std::string_view(argv[index]) == "--") {
      for (++index; index < argc; ++index) driver.command.arguments.emplace_back(argv[index]);
      break;
    }
  }
  if (driver.command.arguments.empty()) {
    const auto *shell = std::getenv("SHELL");
    driver.command.arguments.emplace_back(shell && *shell ? shell : "/bin/sh");
  }
  driver.log.reset(elder_terms::create_terminal_log({std::move(log_settings), {},
      [](bool active) { std::printf("log_active=%d\n", active); std::fflush(stdout); }, {}}));
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "boxrig driver");
  gestament_gtk_assign_accessible_id(window, "driver_window");
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  auto *overlay = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(window), overlay);
  gtk_container_add(GTK_CONTAINER(overlay), content);
  auto prompt_widgets = elder_terms::create_inline_prompt_widgets("driver_ssh_prompt");
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), prompt_widgets.panel);
  driver.prompt = elder_terms::create_inline_prompt_controller(prompt_widgets);
  elder_terms::TerminalSessionCallbacks callbacks;
  callbacks.ssh_prompt = [&driver](const elder_terms::SshUserPrompt &prompt, cardio::cancellation cancellation) {
    return prompt_ssh(&driver, prompt, cancellation);
  };
  if (!ssh.endpoint.address.empty())
    driver.transport = elder_terms::make_ssh_transport_request(ssh, driver.command.encoding, callbacks, options);
  else if (!telnet.address.empty())
    driver.transport = elder_terms::make_telnet_transport_request(telnet, driver.command.encoding, callbacks, options);
  else if (!serial.device.empty()) {
    driver.serial = serial;
    driver.transport = elder_terms::make_serial_transport_request(serial, driver.command.encoding, callbacks);
  }
  auto *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(content), controls, FALSE, FALSE, 0);
  auto *zoom = gtk_button_new_with_label("Zoom in");
  gestament_gtk_assign_accessible_id(zoom, "driver_zoom_in");
  gtk_box_pack_start(GTK_BOX(controls), zoom, FALSE, FALSE, 0);
  auto *copy = gtk_button_new_with_label("Copy selection");
  gestament_gtk_assign_accessible_id(copy, "driver_copy");
  gtk_box_pack_start(GTK_BOX(controls), copy, FALSE, FALSE, 0);
  if (driver.context_loss_control) {
    auto *render_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(content), render_controls, FALSE, FALSE, 0);
    auto *button = gtk_button_new_with_label("Lose GL context");
    gestament_gtk_assign_accessible_id(button, "driver_lose_gl_context");
    gtk_box_pack_start(GTK_BOX(render_controls), button, FALSE, FALSE, 0);
    g_signal_connect(button, "clicked", G_CALLBACK(lose_gl_context), &driver);
  }
  if (driver.links) {
    auto *button = gtk_button_new_with_label("Toggle links");
    gestament_gtk_assign_accessible_id(button, "driver_links_toggle");
    gtk_box_pack_start(GTK_BOX(controls), button, FALSE, FALSE, 0);
    g_signal_connect(button, "clicked", G_CALLBACK(toggle_links), &driver);
  }
  driver.status = gtk_label_new("Starting boxrig");
  gestament_gtk_assign_accessible_id(driver.status, "driver_status");
  gtk_box_pack_start(GTK_BOX(controls), driver.status, FALSE, FALSE, 0);
  if (driver.connection_mode) {
    auto *connections = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(content), connections, FALSE, FALSE, 0);
    auto *stop = gtk_button_new_with_label("Disconnect");
    auto *start = gtk_button_new_with_label("Reconnect");
    gestament_gtk_assign_accessible_id(stop, "driver_disconnect");
    gestament_gtk_assign_accessible_id(start, "driver_reconnect");
    gtk_box_pack_start(GTK_BOX(connections), stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(connections), start, FALSE, FALSE, 0);
    driver.connection_status = gtk_label_new("Starting local command");
    gtk_box_pack_start(GTK_BOX(connections), driver.connection_status, FALSE, FALSE, 0);
    g_signal_connect(stop, "clicked", G_CALLBACK(disconnect), &driver);
    g_signal_connect(start, "clicked", G_CALLBACK(reconnect), &driver);
    driver.break_button = gtk_button_new_with_label("BREAK");
    gestament_gtk_assign_accessible_id(driver.break_button, "driver_break");
    gtk_box_pack_start(GTK_BOX(connections), driver.break_button, FALSE, FALSE, 0);
    g_signal_connect(driver.break_button, "clicked", G_CALLBACK(send_break), &driver);
    if (!driver.sftp_path.empty()) {
      driver.sftp_button = gtk_button_new_with_label("Read SFTP file");
      gestament_gtk_assign_accessible_id(driver.sftp_button, "driver_sftp");
      gtk_box_pack_start(GTK_BOX(connections), driver.sftp_button, FALSE, FALSE, 0);
      g_signal_connect(driver.sftp_button, "clicked", G_CALLBACK(start_sftp), &driver);
    }
    auto *inputs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(content), inputs, FALSE, FALSE, 0);
    const char *input_labels[] = {"Use TRS-80 (Ctrl+F12)", "Use default keys", "Pause input", "Resume input"};
    const char *input_ids[] = {"driver_trs80", "driver_default_input", "driver_pause_input", "driver_resume_input"};
    for (unsigned int index = 0; index < 4; ++index) {
      auto *button = gtk_button_new_with_label(input_labels[index]);
      gestament_gtk_assign_accessible_id(button, input_ids[index]);
      gtk_box_pack_start(GTK_BOX(inputs), button, FALSE, FALSE, 0);
      if (index == 0) g_object_set_data(G_OBJECT(button), "trs80", GINT_TO_POINTER(1));
      if (index == 3) g_object_set_data(G_OBJECT(button), "enabled", GINT_TO_POINTER(1));
      g_signal_connect(button, "clicked", index < 2 ? G_CALLBACK(input_settings_changed) :
          G_CALLBACK(input_enabled_changed), &driver);
    }
    auto *sending = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (driver.serial) {
      auto *serial_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_box_pack_start(GTK_BOX(content), serial_controls, FALSE, FALSE, 0);
      for (const int baud : {9600, 115200}) {
        auto *button = gtk_button_new_with_label((std::to_string(baud) + " baud").c_str());
        gestament_gtk_assign_accessible_id(button, ("driver_baud_" + std::to_string(baud)).c_str());
        g_object_set_data(G_OBJECT(button), "baud", GINT_TO_POINTER(baud));
        gtk_box_pack_start(GTK_BOX(serial_controls), button, FALSE, FALSE, 0);
        g_signal_connect(button, "clicked", G_CALLBACK(baud_changed), &driver);
      }
    }
    gtk_box_pack_start(GTK_BOX(content), sending, FALSE, FALSE, 0);
    auto *paste = gtk_button_new_with_label("Paste");
    gestament_gtk_assign_accessible_id(paste, "driver_paste");
    gtk_box_pack_start(GTK_BOX(sending), paste, FALSE, FALSE, 0);
    g_signal_connect(paste, "clicked", G_CALLBACK(paste_clipboard), &driver);
    for (const auto *encoding : {"UTF-8", "CP932"}) {
      auto *button = gtk_button_new_with_label(encoding);
      gestament_gtk_assign_accessible_id(button, std::string_view(encoding) == "UTF-8" ? "driver_utf8" : "driver_cp932");
      g_object_set_data(G_OBJECT(button), "encoding", const_cast<char *>(encoding));
      gtk_box_pack_start(GTK_BOX(sending), button, FALSE, FALSE, 0);
      g_signal_connect(button, "clicked", G_CALLBACK(encoding_changed), &driver);
    }
    auto *send = gtk_button_new_with_label("Send text");
    gestament_gtk_assign_accessible_id(send, "driver_send_text");
    gtk_box_pack_start(GTK_BOX(sending), send, FALSE, FALSE, 0);
    g_signal_connect(send, "clicked", G_CALLBACK(send_text), &driver);
    auto *cancel = gtk_button_new_with_label("Cancel");
    gestament_gtk_assign_accessible_id(cancel, "driver_cancel_send");
    gtk_box_pack_start(GTK_BOX(sending), cancel, FALSE, FALSE, 0);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_transfer), &driver);
    auto *transfer = gtk_button_new_with_label("Start transfer");
    gestament_gtk_assign_accessible_id(transfer, "driver_transfer");
    gtk_box_pack_start(GTK_BOX(sending), transfer, FALSE, FALSE, 0);
    g_signal_connect(transfer, "clicked", G_CALLBACK(start_transfer), &driver);
  }
  driver.runtime = elder_terms::create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content), GTK_BOX(content), elder_terms::TerminalAmbiguousWidth::narrow);
  if (!driver.runtime) { gtk_widget_destroy(window); return 1; }
  if (driver.links) configure_links(&driver);
  if (!driver.macros.empty()) elder_terms::terminal_runtime_set_macro_rules(driver.runtime,
      std::move(driver.macros), true, [](std::string command, std::vector<std::string> args) {
        for (const auto &arg : args) {
          command.push_back('\0');
          command += arg;
        }
        auto *encoded = g_base64_encode(reinterpret_cast<const guchar *>(command.data()), command.size());
        std::printf("macro=%s\n", encoded);
        g_free(encoded);
        std::fflush(stdout);
      }, [](std::string error) { std::fprintf(stderr, "Macro rejected: %s\n", error.c_str()); });
  if (driver.zmodem_auto) elder_terms::terminal_runtime_set_zmodem_auto_start(driver.runtime,
      [&driver](elder_terms::TerminalTransferDirection, cardio::cancellation cancellation)
          -> cardio::promise<std::optional<elder_terms::TerminalTransferRequest>> {
        cancellation.throw_if_cancellation_requested();
        co_return transfer_request(&driver);
      });
  auto *widget = elder_terms::terminal_runtime_widget(driver.runtime);
  if (boxrig_gtk3_terminal_widget_set_accessibility_enabled(widget, true) != BOXRIG_UI_OK) return 1;
  elder_terms::terminal_runtime_set_host_callbacks(driver.runtime, {
      [&driver](BoxrigGtk3TerminalWidget *attached) { attach_driver_widget(&driver, attached); }, {}, {},
  });
  if (driver.attachment_failed) {
    elder_terms::destroy_terminal_runtime(driver.runtime);
    gtk_widget_destroy(window);
    return 1;
  }
  elder_terms::terminal_runtime_set_changed_callback(driver.runtime, report_state, &driver);
  elder_terms::terminal_runtime_request_input_settings(driver.runtime, driver.input_settings);
  g_signal_connect(zoom, "clicked", G_CALLBACK(zoom_in), &driver);
  g_signal_connect(copy, "clicked", G_CALLBACK(copy_selection), &driver);
  g_signal_connect(window, "delete-event", G_CALLBACK(close_window), &driver);
  const auto signal_source = g_unix_signal_add(SIGTERM, stop_driver, &driver);
  elder_terms::terminal_runtime_set_output_callback(driver.runtime,
      [&driver](std::span<const unsigned char> raw, std::span<const unsigned char> cooked) {
        // Immediate peer data can arrive before the next GTK frame observes
        // connection state. Open this generation's log before its first write.
        observe_log_connection(&driver, elder_terms::terminal_runtime_connection_state(driver.runtime));
        elder_terms::write_terminal_log(driver.log.get(), raw, cooked);
        auto *raw_text = g_base64_encode(raw.data(), raw.size());
        auto *cooked_text = g_base64_encode(cooked.data(), cooked.size());
        std::printf("raw=%s\ncooked=%s\n", raw_text, cooked_text);
        std::fflush(stdout);
        g_free(raw_text);
        g_free(cooked_text);
      });
  std::string output;
  for (unsigned int line = 1; line <= 120; ++line) output += "History line " + std::to_string(line) + "\r\n";
  output += "\033[2J\033[H日本語の端末 — boxrig\r\n\033[1;32mBold green text\033[0m\r\nSelect and copy this text.\r\n";
  const bool fed = !driver.connection_mode ? elder_terms::terminal_runtime_feed(driver.runtime, output) :
      connect_driver(&driver);
  gtk_widget_show_all(window);
  gtk_widget_grab_focus(GTK_WIDGET(widget));
  if (fed) gtk_main();
  g_source_remove(signal_source);
  if (driver.context_loss_hook)
    g_signal_remove_emission_hook(g_signal_lookup("render", GTK_TYPE_GL_AREA), driver.context_loss_hook);
  elder_terms::destroy_terminal_runtime(driver.runtime);
  gtk_widget_destroy(window);
  return fed ? 0 : 1;
}
