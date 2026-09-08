#include "../../src/terminal-session.h"
#include "../../src/terminal-sessions/local-session/local-session.h"
#include "../../src/terminal-sessions/serial-session/serial-session.h"
#include "../../src/terminal-sessions/ssh-session/ssh-session.h"
#include "../../src/terminal-sessions/telnet-session/telnet-session.h"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace elder_terms {

struct BackendRecord {
  TerminalSessionCallbacks callbacks{};
  TerminalConnectionSettings settings{};
  TerminalTextSettings text_settings{};
  std::string known_hosts;
  cardio::promise_source<void> finished;
  cardio::promise<void> work = finished.get_promise();
  std::optional<TerminalTransferRequest> transfer;
  glong columns = 0;
  glong rows = 0;
  int starts = 0;
  bool stopped = false;
  bool joined = false;
  bool destroyed = false;
  bool destroyed_before_join = false;
  bool zmodem = false;
};

static std::vector<std::shared_ptr<BackendRecord>> records;
static cardio::promise_source<void> *backend_created = nullptr;

class ControlledSession final : public TerminalSession {
  std::shared_ptr<BackendRecord> record;

public:
  explicit ControlledSession(std::shared_ptr<BackendRecord> record)
      : record(std::move(record)) {}
  ~ControlledSession() override {
    record->destroyed = true;
    record->destroyed_before_join = record->starts != 0 && !record->joined;
  }
  bool start() override {
    ++record->starts;
    if (record->callbacks.connection_phase) {
      record->callbacks.connection_phase(TerminalSessionConnectionPhase::connected);
    }
    return true;
  }
  void stop() override { record->stopped = true; }
  cardio::promise<void> wait_stopped_async() override {
    co_await record->work;
    record->joined = true;
  }
  void resize(glong columns, glong rows) override {
    record->columns = columns;
    record->rows = rows;
  }
  std::string connection_detail() const override { return "controlled"; }
  void set_zmodem_autostart(bool enabled) override { record->zmodem = enabled; }
  bool start_transfer(TerminalTransferRequest request) override {
    record->transfer = std::move(request);
    return true;
  }
  void apply_connection_profile(const TerminalConnectionProfile &profile) override {
    record->text_settings = profile.text_settings;
  }
};

static std::unique_ptr<TerminalSession> create_controlled_session(
    TerminalConnectionSettings settings, TerminalTextSettings text_settings,
    TerminalSessionCallbacks callbacks, std::string known_hosts) {
  auto record = std::make_shared<BackendRecord>();
  record->settings = std::move(settings);
  record->text_settings = std::move(text_settings);
  record->callbacks = std::move(callbacks);
  record->known_hosts = std::move(known_hosts);
  records.push_back(record);
  if (backend_created != nullptr) {
    (void)backend_created->try_resolve();
  }
  return std::make_unique<ControlledSession>(std::move(record));
}

std::unique_ptr<TerminalSession> create_terminal_local_shell_session(
    GtkWidget *, LocalShellConnectionSettings settings,
    TerminalTextSettings text_settings, TerminalSessionCallbacks callbacks) {
  return create_controlled_session(std::move(settings), std::move(text_settings),
                                   std::move(callbacks), {});
}

std::unique_ptr<TerminalSession> create_terminal_serial_session(
    GtkWidget *, SerialConnectionSettings settings,
    TerminalTextSettings text_settings, TerminalSessionCallbacks callbacks) {
  return create_controlled_session(std::move(settings), std::move(text_settings),
                                   std::move(callbacks), {});
}

std::unique_ptr<TerminalSession> create_terminal_telnet_session(
    GtkWidget *, TelnetConnectionSettings settings,
    TerminalTextSettings text_settings, TerminalSessionCallbacks callbacks) {
  return create_controlled_session(std::move(settings), std::move(text_settings),
                                   std::move(callbacks), {});
}

std::unique_ptr<TerminalSession> create_terminal_ssh_session(
    GtkWidget *, SshConnectionSettings settings,
    TerminalTextSettings text_settings, TerminalSessionCallbacks callbacks,
    SshChannelConnectionOptions options) {
  return create_controlled_session(std::move(settings), std::move(text_settings),
                                   std::move(callbacks), std::move(options.known_hosts_file));
}

static void expect_true(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_reconnection(TerminalConnectionKind kind,
                                               bool stop_before_restart) {
  TerminalConnectionProfile profile{};
  profile.kind = kind;
  profile.name = "first";
  if (kind == TerminalConnectionKind::ssh) {
    SshConnectionSettings settings{};
    settings.endpoint.address = "first.example";
    profile.settings = settings;
  } else {
    TelnetConnectionSettings settings{};
    settings.address = "first.example";
    profile.settings = settings;
  }

  TerminalSessionState *state = nullptr;
  cardio::promise_source<void> ready;
  auto ready_task = ready.get_promise();
  int ended = 0;
  int activity = 0;
  int transfer_notifications = 0;
  auto phase = TerminalSessionConnectionPhase::disconnected;
  TerminalSessionCallbacks callbacks{};
  callbacks.ended = [&]() { ++ended; };
  callbacks.activity = [&](ActivityIndicatorId) { ++activity; };
  callbacks.connection_phase = [&](TerminalSessionConnectionPhase value) { phase = value; };
  callbacks.reconnect_state_changed = [&]() {
    if (terminal_session_can_reconnect(state)) (void)ready.try_resolve();
  };
  state = create_terminal_session(nullptr, profile, std::move(callbacks),
                                  {.ssh_known_hosts_file = "isolated-known-hosts"});
  std::exception_ptr failure;
  std::optional<cardio::promise<void>> shutdown;
  try {
    expect_true(start_terminal_session(state), "initial start must succeed");
    auto old = records.back();
    expect_true(!old->callbacks.zmodem_auto_start && !old->callbacks.ssh_prompt,
                "absent optional handlers must remain absent for backend capability checks");
    expect_true(!reconnect_terminal_session(state), "active connection cannot restart");
    TerminalTransferRequest request{};
    request.active = [&](bool) { ++transfer_notifications; };
    expect_true(start_terminal_session_transfer(state, std::move(request)),
                "controlled transfer should start");
    profile.name = "updated";
    profile.text_settings.encoding = "ISO-8859-1";
    if (kind == TerminalConnectionKind::ssh) {
      std::get<SshConnectionSettings>(profile.settings).endpoint.address = "updated.example";
    } else {
      std::get<TelnetConnectionSettings>(profile.settings).address = "updated.example";
    }
    apply_terminal_session_connection_profile(state, profile);
    resize_terminal_session(state, 95, 31);
    set_terminal_session_zmodem_autostart(state, true);

    old->callbacks.connection_phase(TerminalSessionConnectionPhase::disconnected);
    old->callbacks.ended();
    old->callbacks.ended();
    expect_true(ended == 1, "one natural end must reach the application");
    expect_true(!reconnect_terminal_session(state), "unfinished work must prevent restart");
    co_await cardio::resolved();
    expect_true(!old->destroyed && !terminal_session_can_reconnect(state),
                "backend must survive while its work is pending");
    old->finished.resolve();
    co_await ready_task;
    expect_true(old->stopped && old->joined, "ready requires stopped and joined work");
    expect_true(reconnect_terminal_session(state), "quiescent backend can restart");
    expect_true(!reconnect_terminal_session(state), "repeated request must be rejected");
    expect_true(!old->destroyed, "restart must leave the requesting stack first");

    if (stop_before_restart) {
      stop_terminal_session(state);
      co_await stop_terminal_session_async(state);
      expect_true(records.size() == 1, "closing during pending restart must not reopen");
    } else {
      cardio::promise_source<void> created;
      auto created_task = created.get_promise();
      backend_created = &created;
      co_await created_task;
      backend_created = nullptr;
      expect_true(records.size() == 2, "exactly one replacement must be created");
      auto current = records.back();
      expect_true(old->destroyed && !old->destroyed_before_join,
                  "old backend may only be destroyed after joining");
      expect_true(current->starts == 1 && current->columns == 95 && current->rows == 31,
                  "replacement starts once with current grid size");
      expect_true(current->zmodem && current->text_settings.encoding == "ISO-8859-1",
                  "replacement must use current runtime settings");
      if (kind == TerminalConnectionKind::ssh) {
        expect_true(current->known_hosts == "isolated-known-hosts" &&
                        std::get<SshConnectionSettings>(current->settings).endpoint.address == "updated.example",
                    "SSH must retain overrides and use the latest endpoint");
      } else {
        expect_true(std::get<TelnetConnectionSettings>(current->settings).address == "updated.example",
                    "TELNET must use the latest endpoint");
      }
      old->callbacks.activity(ActivityIndicatorId::rd);
      old->callbacks.connection_phase(TerminalSessionConnectionPhase::disconnected);
      old->callbacks.ended();
      old->transfer->active(false);
      expect_true(activity == 0 && ended == 1 && transfer_notifications == 0 &&
                      phase == TerminalSessionConnectionPhase::connected,
                  "old generation notifications must not affect the new connection");
      stop_terminal_session(state);
      shutdown.emplace(stop_terminal_session_async(state));
      co_await cardio::resolved();
      expect_true(!shutdown->is_ready() && !current->destroyed,
                  "shutdown must keep pending backend work alive");
      current->callbacks.activity(ActivityIndicatorId::sd);
      expect_true(activity == 0, "shutdown must suppress notifications to the closed UI");
      current->finished.resolve();
      co_await *shutdown;
    }
    expect_true(!terminal_session_can_reconnect(state), "stopped manager cannot reconnect");
  } catch (...) {
    failure = std::current_exception();
  }
  backend_created = nullptr;
  (void)ready.try_resolve();
  for (const auto &record : records) (void)record->finished.try_resolve();
  if (shutdown.has_value()) {
    co_await *shutdown;
  } else {
    co_await stop_terminal_session_async(state);
  }
  destroy_terminal_session(state);
  for (const auto &record : records) {
    expect_true(record->destroyed && !record->destroyed_before_join,
                "all started backends must be joined before destruction");
  }
  records.clear();
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<void> run_tests() {
  for (const auto kind : {TerminalConnectionKind::ssh, TerminalConnectionKind::telnet}) {
    co_await test_reconnection(kind, false);
    co_await test_reconnection(kind, true);
  }
}

static cardio::promise<void> run_checked(bool &succeeded,
                                        cardio::dispatcher_group_glib &group) {
  try {
    co_await run_tests();
    succeeded = true;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  group.shutdown();
}

} // namespace elder_terms

int main() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  bool succeeded = false;
  auto task = elder_terms::run_checked(succeeded, group);
  dispatcher.park();
  return succeeded ? 0 : 1;
}
