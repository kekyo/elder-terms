#include "terminal-log.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <glib.h>

namespace elder_terms {

enum class TerminalLogCommandKind {
  open,
  write,
  close,
};

struct TerminalLogCommand {
  TerminalLogCommandKind kind = TerminalLogCommandKind::close;
  std::filesystem::path path;
  std::vector<unsigned char> bytes;
};

struct TerminalLogState {
  TerminalLogSettings settings;
  TerminalLogNowCallback now;
  TerminalLogActiveCallback active_callback;
  TerminalLogWarningCallback warning_callback;
  std::optional<cardio::io_uring> io;
  std::optional<cardio::promise<void>> processor_task;
  std::deque<TerminalLogCommand> commands;
  int fd = -1;
  bool connected = false;
  bool accepting = false;
  bool writer_active = false;
  bool processing = false;
  bool stopping = false;
};

static std::string replace_all(std::string value, const std::string &needle,
                               const std::string &replacement) {
  std::size_t offset = 0;
  while ((offset = value.find(needle, offset)) != std::string::npos) {
    value.replace(offset, needle.size(), replacement);
    offset += replacement.size();
  }
  return value;
}

static std::string format_number(int value, int width) {
  std::array<char, 32> buffer{};
  const int written =
      std::snprintf(buffer.data(), buffer.size(), "%0*d", width, value);
  if (written < 0 || static_cast<std::size_t>(written) >= buffer.size()) {
    throw std::runtime_error("failed to format terminal log timestamp");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

static std::filesystem::path
expanded_log_base_directory(const std::string &configured) {
  static constexpr std::string_view home_variable = "$HOME";
  static constexpr std::string_view documents_variable = "{XDG_DOCUMENTS}";
  const auto uses_variable = [&configured](std::string_view variable) {
    return configured == variable ||
           (configured.starts_with(variable) &&
            configured.size() > variable.size() &&
            configured[variable.size()] == '/');
  };
  const auto append_suffix = [&configured](
                                 const std::filesystem::path &base,
                                 std::string_view variable) {
    const std::string_view suffix(configured.data() + variable.size(),
                                  configured.size() - variable.size());
    const std::size_t relative_start = suffix.find_first_not_of('/');
    return relative_start == std::string_view::npos
               ? base
               : base / suffix.substr(relative_start);
  };
  const auto home_directory = []() {
    const char *home = g_get_home_dir();
    if (home == nullptr || home[0] == '\0') {
      throw std::runtime_error(
          "failed to resolve the user home for terminal logging");
    }
    return std::filesystem::path(home);
  };

  if (uses_variable(documents_variable)) {
    const char *documents =
        g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    const std::filesystem::path base =
        documents != nullptr && documents[0] != '\0'
            ? std::filesystem::path(documents)
            : home_directory();
    return append_suffix(base, documents_variable);
  }

  if (uses_variable(home_variable)) {
    return append_suffix(home_directory(), home_variable);
  }
  return configured.empty() ? std::filesystem::path(".")
                            : std::filesystem::path(configured);
}

static std::filesystem::path formatted_log_path(
    const TerminalLogSettings &settings,
    std::chrono::system_clock::time_point now) {
  std::string reason;
  if (!terminal_log_file_name_format_is_valid(settings.file_name_format,
                                               &reason)) {
    throw std::invalid_argument("invalid terminal log file name format: " +
                                reason);
  }

  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  if (::localtime_r(&seconds, &local) == nullptr) {
    throw std::runtime_error("failed to resolve local terminal log time");
  }
  const auto elapsed_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count();
  const int millisecond = static_cast<int>(
      (elapsed_milliseconds % 1000 + 1000) % 1000);

  const std::string date = format_number(local.tm_year + 1900, 4) +
                           format_number(local.tm_mon + 1, 2) +
                           format_number(local.tm_mday, 2);
  const std::string time = format_number(local.tm_hour, 2) +
                           format_number(local.tm_min, 2) +
                           format_number(local.tm_sec, 2);
  std::string relative = replace_all(settings.file_name_format,
                                     "{YYYYMMDD}", date);
  relative = replace_all(std::move(relative), "{hhmmss}", time);
  relative = replace_all(std::move(relative), "{fff}",
                         format_number(millisecond, 3));

  const std::filesystem::path base =
      expanded_log_base_directory(settings.base_directory);
  return (base / std::filesystem::path(relative)).lexically_normal();
}

static void notify_warning(TerminalLogState *state,
                           const std::string &warning) {
  if (state->warning_callback) {
    state->warning_callback(warning);
    return;
  }
  std::cerr << warning << '\n';
}

static void set_writer_active(TerminalLogState *state, bool active) {
  if (state->writer_active == active) {
    return;
  }
  state->writer_active = active;
  if (state->active_callback) {
    state->active_callback(active);
  }
}

static cardio::promise<void> create_directories_async(
    cardio::io_uring &io, const std::filesystem::path &directory) {
  const std::filesystem::path normalized = directory.lexically_normal();
  std::filesystem::path current = normalized.root_path();
  for (const std::filesystem::path &component : normalized) {
    if (component == normalized.root_name() ||
        component == normalized.root_directory() || component == ".") {
      continue;
    }
    current /= component;
    try {
      co_await cardio::io_urings::mkdir(io, current.string(), 0755);
    } catch (const std::system_error &error) {
      if (error.code().value() != EEXIST) {
        throw;
      }
    }
  }
}

static cardio::promise<void> close_log_file_async(TerminalLogState *state) {
  const int fd = std::exchange(state->fd, -1);
  if (fd < 0) {
    set_writer_active(state, false);
    co_return;
  }

  try {
    co_await cardio::io_urings::close(*state->io, fd);
  } catch (const std::exception &error) {
    (void)::close(fd);
    notify_warning(state, "Warning: failed to close terminal log: " +
                              std::string(error.what()));
  }
  set_writer_active(state, false);
}

static cardio::promise<void> open_log_file_async(
    TerminalLogState *state, const std::filesystem::path &path) {
  co_await close_log_file_async(state);
  try {
    if (!state->io.has_value()) {
      state->io.emplace(32);
    }
    co_await create_directories_async(*state->io, path.parent_path());
    state->fd = co_await cardio::io_urings::open(
        *state->io, path.string(),
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    set_writer_active(state, true);
  } catch (const std::exception &error) {
    state->fd = -1;
    set_writer_active(state, false);
    notify_warning(state, "Warning: failed to open terminal log " +
                              path.string() + ": " + error.what());
  }
}

static cardio::promise<void> write_log_bytes_async(
    TerminalLogState *state, const std::vector<unsigned char> &bytes) {
  if (state->fd < 0 || bytes.empty()) {
    co_return;
  }

  std::string failure;
  try {
    std::span<const unsigned char> remaining(bytes.data(), bytes.size());
    while (!remaining.empty()) {
      const std::size_t written = co_await cardio::io_urings::write(
          *state->io, state->fd, std::as_bytes(remaining));
      if (written == 0 || written > remaining.size()) {
        throw std::runtime_error("terminal log write made no progress");
      }
      remaining = remaining.subspan(written);
    }
  } catch (const std::exception &error) {
    failure = error.what();
  }
  if (!failure.empty()) {
    notify_warning(state,
                   "Warning: failed to write terminal log: " + failure);
    co_await close_log_file_async(state);
  }
}

static cardio::promise<void> process_log_commands_async(
    TerminalLogState *state) {
  try {
    while (!state->commands.empty()) {
      TerminalLogCommand command = std::move(state->commands.front());
      state->commands.pop_front();
      if (command.kind == TerminalLogCommandKind::open) {
        co_await open_log_file_async(state, command.path);
      } else if (command.kind == TerminalLogCommandKind::write) {
        co_await write_log_bytes_async(state, command.bytes);
      } else {
        co_await close_log_file_async(state);
      }
    }
  } catch (const std::exception &error) {
    notify_warning(state, "Warning: terminal log processing failed: " +
                              std::string(error.what()));
    if (state->fd >= 0) {
      (void)::close(std::exchange(state->fd, -1));
    }
    set_writer_active(state, false);
  }
  state->processing = false;
}

static void start_log_processor(TerminalLogState *state) {
  if (state->processing) {
    return;
  }
  state->processing = true;
  state->processor_task.reset();
  state->processor_task.emplace(process_log_commands_async(state));
}

static void enqueue_log_command(TerminalLogState *state,
                                TerminalLogCommand command) {
  state->commands.push_back(std::move(command));
  start_log_processor(state);
}

static void begin_connection_log(TerminalLogState *state) {
  if (!state->settings.enabled || state->stopping) {
    state->accepting = false;
    return;
  }

  try {
    const std::filesystem::path path =
        formatted_log_path(state->settings, state->now());
    state->accepting = true;
    enqueue_log_command(state, TerminalLogCommand{
                                   .kind = TerminalLogCommandKind::open,
                                   .path = path,
                                   .bytes = {},
                               });
  } catch (const std::exception &error) {
    state->accepting = false;
    notify_warning(state, "Warning: failed to format terminal log path: " +
                              std::string(error.what()));
  }
}

TerminalLogState *create_terminal_log(TerminalLogOptions options) {
  auto *state = new TerminalLogState();
  state->settings = std::move(options.settings);
  state->now = options.now
                   ? std::move(options.now)
                   : TerminalLogNowCallback{
                         []() { return std::chrono::system_clock::now(); }};
  state->active_callback = std::move(options.active);
  state->warning_callback = std::move(options.warning);
  return state;
}

void apply_terminal_log_settings(TerminalLogState *state,
                                 TerminalLogSettings settings) {
  if (state == nullptr || state->stopping || state->settings == settings) {
    return;
  }

  state->settings = std::move(settings);
  if (!state->connected) {
    return;
  }

  state->accepting = false;
  enqueue_log_command(state, TerminalLogCommand{
                                 .kind = TerminalLogCommandKind::close,
                                 .path = {},
                                 .bytes = {},
                             });
  begin_connection_log(state);
}

void set_terminal_log_connection_active(TerminalLogState *state,
                                        bool active) {
  if (state == nullptr || state->stopping || state->connected == active) {
    return;
  }

  state->connected = active;
  if (active) {
    begin_connection_log(state);
    return;
  }

  state->accepting = false;
  enqueue_log_command(state, TerminalLogCommand{
                                 .kind = TerminalLogCommandKind::close,
                                 .path = {},
                                 .bytes = {},
                             });
}

void write_terminal_log(TerminalLogState *state,
                        std::span<const unsigned char> raw_bytes,
                        std::span<const unsigned char> cooked_bytes) {
  if (state == nullptr || !state->accepting || state->stopping) {
    return;
  }

  const std::span<const unsigned char> selected =
      state->settings.mode == TerminalLogMode::cooked ? cooked_bytes
                                                      : raw_bytes;
  if (selected.empty()) {
    return;
  }
  enqueue_log_command(state, TerminalLogCommand{
                                 .kind = TerminalLogCommandKind::write,
                                 .path = {},
                                 .bytes = {selected.begin(), selected.end()},
                             });
}

cardio::promise<void> stop_terminal_log_async(TerminalLogState *state) {
  if (state == nullptr) {
    co_return;
  }

  if (!state->stopping) {
    state->stopping = true;
    state->connected = false;
    state->accepting = false;
    enqueue_log_command(state, TerminalLogCommand{
                                   .kind = TerminalLogCommandKind::close,
                                   .path = {},
                                   .bytes = {},
                               });
  }
  if (state->processor_task.has_value()) {
    co_await *state->processor_task;
  }
}

void destroy_terminal_log(TerminalLogState *state) {
  if (state == nullptr) {
    return;
  }
  if (state->processing) {
    notify_warning(state,
                   "Warning: terminal log destroyed before stop completed");
    return;
  }
  if (state->fd >= 0) {
    (void)::close(state->fd);
  }
  delete state;
}

} // namespace elder_terms
