#include "../../src/terminal-log.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>
#include <unistd.h>

namespace elder_terms_terminal_log_test {

class ScopedEnvironment {
public:
  ScopedEnvironment(const char *name, const std::string &value)
      : name_(name) {
    const char *old_value = g_getenv(name);
    if (old_value != nullptr) {
      old_value_ = old_value;
    }
    g_setenv(name, value.c_str(), TRUE);
  }

  ~ScopedEnvironment() {
    if (old_value_.has_value()) {
      g_setenv(name_.c_str(), old_value_->c_str(), TRUE);
    } else {
      g_unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::filesystem::path temporary_directory(const std::string &name) {
  return std::filesystem::temp_directory_path() /
         ("elder-terms-terminal-log-" + std::to_string(::getpid()) + "-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + name);
}

static std::chrono::system_clock::time_point local_time_point(
    int year, int month, int day, int hour, int minute, int second,
    int millisecond) {
  std::tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  value.tm_isdst = -1;
  const std::time_t seconds = std::mktime(&value);
  if (seconds == static_cast<std::time_t>(-1)) {
    throw std::runtime_error("failed to create terminal log test time");
  }
  return std::chrono::system_clock::from_time_t(seconds) +
         std::chrono::milliseconds(millisecond);
}

static std::span<const unsigned char> bytes(const std::string &value) {
  return std::span<const unsigned char>(
      reinterpret_cast<const unsigned char *>(value.data()), value.size());
}

static std::string read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  expect_true(file.good(), "failed to open terminal log test output: " +
                               path.string());
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

static void run_until_terminal_log_stops(
    elder_terms::TerminalLogState *log,
    const std::function<void()> &enqueue_operations) {
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  std::exception_ptr async_error;
  auto task = [&]() -> cardio::promise<void> {
    try {
      enqueue_operations();
      co_await elder_terms::stop_terminal_log_async(log);
    } catch (...) {
      async_error = std::current_exception();
    }
    dispatcher_group.shutdown();
  }();

  dispatcher.park();
  task.unsafe_result();
  if (async_error) {
    std::rethrow_exception(async_error);
  }
}

static void write_single_raw_log(elder_terms::TerminalLogSettings settings,
                                 const std::string &content,
                                 elder_terms::TerminalLogNowCallback now) {
  elder_terms::TerminalLogState *log = elder_terms::create_terminal_log({
      .settings = settings,
      .now = now,
      .active = nullptr,
      .warning = nullptr,
  });

  run_until_terminal_log_stops(log, [&]() {
    elder_terms::set_terminal_log_connection_active(log, true);
    elder_terms::write_terminal_log(log, bytes(content), bytes("unused"));
    elder_terms::set_terminal_log_connection_active(log, false);
  });
  elder_terms::destroy_terminal_log(log);
}

static void test_user_directory_base_paths_are_expanded() {
  const std::filesystem::path root = temporary_directory("user-directories");
  const std::filesystem::path config_home = root / "config";
  const std::filesystem::path documents = root / "XDG Documents";
  const std::filesystem::path downloads = root / "XDG Downloads";
  const std::filesystem::path home(g_get_home_dir());
  const std::filesystem::path home_log_directory =
      home / root.filename();
  const std::filesystem::path documents_fallback_directory =
      home / "Documents" / root.filename();
  const std::filesystem::path downloads_fallback_directory =
      home / "Downloads" / root.filename();

  try {
    std::filesystem::create_directories(config_home);
    std::filesystem::create_directories(documents);
    std::filesystem::create_directories(downloads);
    {
      ScopedEnvironment config_env("XDG_CONFIG_HOME", config_home.string());
      g_reload_user_special_dirs_cache();

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory =
                  "${documents}/" + root.filename().string(),
              .file_name_format = "documents-fallback.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "documents fallback", nullptr);
      expect_true(
          read_file(documents_fallback_directory /
                    "documents-fallback.log") == "documents fallback",
          "missing XDG Documents should fall back to $HOME/Documents");

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory =
                  "${downloads}/" + root.filename().string(),
              .file_name_format = "downloads-fallback.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "downloads fallback", nullptr);
      expect_true(
          read_file(downloads_fallback_directory /
                    "downloads-fallback.log") == "downloads fallback",
          "missing XDG Downloads should fall back to $HOME/Downloads");

      {
        std::ofstream user_dirs(config_home / "user-dirs.dirs");
        user_dirs << "XDG_DOCUMENTS_DIR=\"" << documents.string()
                  << "\"\n"
                  << "XDG_DOWNLOAD_DIR=\"" << downloads.string()
                  << "\"\n";
      }
      g_reload_user_special_dirs_cache();
      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory = "${documents}/logs",
              .file_name_format = "documents.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "documents", nullptr);
      expect_true(read_file(documents / "logs" / "documents.log") ==
                      "documents",
                  "XDG Documents token should resolve to the configured "
                  "directory");

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory = "${downloads}/logs",
              .file_name_format = "downloads.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "downloads", nullptr);
      expect_true(read_file(downloads / "logs" / "downloads.log") ==
                      "downloads",
                  "XDG Downloads token should resolve to the configured "
                  "directory");

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory = "${home}/" + root.filename().string(),
              .file_name_format = "home.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "home", nullptr);
      expect_true(read_file(home_log_directory / "home.log") == "home",
                  "home token should resolve to the user home");

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory = root.string(),
              .file_name_format =
                  "${documents}/logs/documents-from-format.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "documents format", nullptr);
      expect_true(
          read_file(documents / "logs" / "documents-from-format.log") ==
              "documents format",
          "XDG Documents token should be usable in the file name format");

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory = root.string(),
              .file_name_format =
                  "${downloads}/logs/downloads-from-format.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "downloads format", nullptr);
      expect_true(
          read_file(downloads / "logs" / "downloads-from-format.log") ==
              "downloads format",
          "XDG Downloads token should be usable in the file name format");

      write_single_raw_log(
          elder_terms::TerminalLogSettings{
              .enabled = true,
              .base_directory = root.string(),
              .file_name_format =
                  "${home}/" + root.filename().string() +
                  "/home-from-format.log",
              .connection_name = "fixture",
              .mode = elder_terms::TerminalLogMode::raw,
          },
          "home format", nullptr);
      expect_true(read_file(home_log_directory / "home-from-format.log") ==
                      "home format",
                  "home token should be usable in the file name format");
    }

    g_reload_user_special_dirs_cache();
    std::filesystem::remove_all(home_log_directory);
    std::filesystem::remove_all(documents_fallback_directory);
    std::filesystem::remove_all(downloads_fallback_directory);
    std::filesystem::remove_all(root);
  } catch (...) {
    g_reload_user_special_dirs_cache();
    std::filesystem::remove_all(home_log_directory);
    std::filesystem::remove_all(documents_fallback_directory);
    std::filesystem::remove_all(downloads_fallback_directory);
    std::filesystem::remove_all(root);
    throw;
  }
}

static void test_connection_name_path_placeholder_is_sanitized_once() {
  const std::filesystem::path root = temporary_directory("connection-name");
  std::filesystem::create_directories(root);

  try {
    write_single_raw_log(
        elder_terms::TerminalLogSettings{
            .enabled = true,
            .base_directory = root.string(),
            .file_name_format = "${name}/session.log",
            .connection_name = "Tokyo/../Lab",
            .mode = elder_terms::TerminalLogMode::raw,
        },
        "slash", nullptr);
    expect_true(read_file(root / "Tokyo_.._Lab" / "session.log") == "slash",
                "slashes in a connection name should not create path "
                "components");

    write_single_raw_log(
        elder_terms::TerminalLogSettings{
            .enabled = true,
            .base_directory = root.string() + "/${name}",
            .file_name_format = "session.log",
            .connection_name = "..",
            .mode = elder_terms::TerminalLogMode::raw,
        },
        "dot", nullptr);
    expect_true(read_file(root / "__" / "session.log") == "dot",
                "a dot-only connection name should not traverse the log "
                "directory");

    write_single_raw_log(
        elder_terms::TerminalLogSettings{
            .enabled = true,
            .base_directory = root.string(),
            .file_name_format = "${name}.log",
            .connection_name = "literal-${YYYY}",
            .mode = elder_terms::TerminalLogMode::raw,
        },
        "literal", nullptr);
    expect_true(read_file(root / "literal-${YYYY}.log") == "literal",
                "placeholders introduced by a connection name should not be "
                "expanded recursively");

    write_single_raw_log(
        elder_terms::TerminalLogSettings{
            .enabled = true,
            .base_directory = root.string(),
            .file_name_format = "${name}.log",
            .connection_name = std::string("A\0B", 3),
            .mode = elder_terms::TerminalLogMode::raw,
        },
        "nul", nullptr);
    expect_true(read_file(root / "A_B.log") == "nul",
                "NUL bytes in a connection name should be sanitized");

    write_single_raw_log(
        elder_terms::TerminalLogSettings{
            .enabled = true,
            .base_directory = root.string(),
            .file_name_format = "${name}.log",
            .connection_name = "",
            .mode = elder_terms::TerminalLogMode::raw,
        },
        "empty", nullptr);
    expect_true(read_file(root / "_.log") == "empty",
                "an empty connection name should produce a safe file name");

    std::filesystem::remove_all(root);
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
}

static void test_connection_boundaries_reopen_formatted_paths_and_modes() {
  const std::filesystem::path root = temporary_directory("connections");
  std::filesystem::create_directories(root);
  const std::vector<std::chrono::system_clock::time_point> times{
      local_time_point(2026, 7, 20, 12, 34, 56, 123),
      local_time_point(2026, 7, 20, 12, 35, 1, 456),
  };
  std::size_t time_index = 0;
  std::vector<bool> active_states;
  std::vector<std::string> warnings;

  elder_terms::TerminalLogSettings settings{
      .enabled = true,
      .base_directory = root.string() + "/${YYYY/MM/DD}",
      .file_name_format = "${hh}-${mm}-${ss}_${fff}.txt",
      .connection_name = "fixture",
      .mode = elder_terms::TerminalLogMode::raw,
  };
  elder_terms::TerminalLogState *log = elder_terms::create_terminal_log({
      .settings = settings,
      .now = [&times, &time_index]() { return times.at(time_index++); },
      .active = [&active_states](bool active) {
        active_states.push_back(active);
      },
      .warning = [&warnings](const std::string &warning) {
        warnings.push_back(warning);
      },
  });

  run_until_terminal_log_stops(log, [&]() {
    elder_terms::set_terminal_log_connection_active(log, true);
    elder_terms::write_terminal_log(log, bytes("raw-first"),
                                    bytes("cooked-first"));
    elder_terms::set_terminal_log_connection_active(log, false);

    settings.mode = elder_terms::TerminalLogMode::cooked;
    settings.base_directory = root.string() + "/${YYYY}-${MM}-${DD}";
    settings.file_name_format =
        "${YYYY-MM-DD}/${hh:mm:ss}_${fff}.txt";
    elder_terms::apply_terminal_log_settings(log, settings);
    elder_terms::set_terminal_log_connection_active(log, true);
    elder_terms::write_terminal_log(log, bytes("raw-second"),
                                    bytes("cooked-second"));
    elder_terms::set_terminal_log_connection_active(log, false);
  });
  elder_terms::destroy_terminal_log(log);

  expect_true(warnings.empty(),
              "successful terminal logging should not emit warnings");
  expect_true(active_states == std::vector<bool>({true, false, true, false}),
              "LOG activity should follow both opened connection files");
  expect_true(read_file(root / "2026" / "07" / "20" /
                        "12-34-56_123.txt") ==
                  "raw-first",
              "raw mode should expand split time tokens and date separators "
              "inside the base placeholder");
  expect_true(read_file(root / "2026-07-20" / "2026-07-20" /
                        "12:35:01_456.txt") ==
                  "cooked-second",
              "cooked mode should expand split base tokens and separators "
              "inside file placeholders");

  std::filesystem::remove_all(root);
}

static void test_settings_toggle_logging_while_connected() {
  const std::filesystem::path root = temporary_directory("runtime-toggle");
  std::filesystem::create_directories(root);
  std::vector<bool> active_states;
  std::vector<std::string> warnings;
  elder_terms::TerminalLogSettings settings{
      .enabled = false,
      .base_directory = root.string(),
      .file_name_format = "runtime.log",
      .mode = elder_terms::TerminalLogMode::raw,
  };
  elder_terms::TerminalLogState *log = elder_terms::create_terminal_log({
      .settings = settings,
      .now = []() { return std::chrono::system_clock::now(); },
      .active = [&active_states](bool active) {
        active_states.push_back(active);
      },
      .warning = [&warnings](const std::string &warning) {
        warnings.push_back(warning);
      },
  });

  run_until_terminal_log_stops(log, [&]() {
    elder_terms::set_terminal_log_connection_active(log, true);
    elder_terms::write_terminal_log(log, bytes("before"), bytes("unused"));

    settings.enabled = true;
    elder_terms::apply_terminal_log_settings(log, settings);
    elder_terms::write_terminal_log(log, bytes("recorded"), bytes("unused"));

    settings.enabled = false;
    elder_terms::apply_terminal_log_settings(log, settings);
    elder_terms::write_terminal_log(log, bytes("after"), bytes("unused"));
  });
  elder_terms::destroy_terminal_log(log);

  expect_true(warnings.empty(),
              "runtime logging toggles should not emit warnings");
  expect_true(active_states == std::vector<bool>({true, false}),
              "runtime logging toggles should open and close the log");
  expect_true(read_file(root / "runtime.log") == "recorded",
              "only output received while runtime logging is enabled should "
              "be recorded");
  std::filesystem::remove_all(root);
}

static void test_disabled_logging_creates_no_file() {
  const std::filesystem::path root = temporary_directory("disabled");
  std::filesystem::create_directories(root);
  int now_calls = 0;
  std::vector<bool> active_states;

  elder_terms::TerminalLogState *log = elder_terms::create_terminal_log({
      .settings = elder_terms::TerminalLogSettings{
          .enabled = false,
          .base_directory = root.string(),
          .file_name_format = "disabled.log",
          .mode = elder_terms::TerminalLogMode::raw,
      },
      .now = [&now_calls]() {
        ++now_calls;
        return std::chrono::system_clock::now();
      },
      .active = [&active_states](bool active) {
        active_states.push_back(active);
      },
      .warning = nullptr,
  });

  run_until_terminal_log_stops(log, [&]() {
    elder_terms::set_terminal_log_connection_active(log, true);
    elder_terms::write_terminal_log(log, bytes("raw"), bytes("cooked"));
    elder_terms::set_terminal_log_connection_active(log, false);
  });
  elder_terms::destroy_terminal_log(log);

  expect_true(now_calls == 0,
              "disabled logging should not evaluate its path format");
  expect_true(active_states.empty(),
              "disabled logging should not activate the LOG indicator");
  expect_true(!std::filesystem::exists(root / "disabled.log"),
              "disabled logging should not create a log file");
  std::filesystem::remove_all(root);
}

static void test_open_failure_warns_and_stops_safely() {
  const std::filesystem::path root = temporary_directory("open-failure");
  std::filesystem::create_directories(root);
  const std::filesystem::path base_file = root / "not-a-directory";
  {
    std::ofstream file(base_file);
    file << "occupied";
  }
  std::vector<bool> active_states;
  std::vector<std::string> warnings;

  elder_terms::TerminalLogState *log = elder_terms::create_terminal_log({
      .settings = elder_terms::TerminalLogSettings{
          .enabled = true,
          .base_directory = base_file.string(),
          .file_name_format = "session.log",
          .mode = elder_terms::TerminalLogMode::raw,
      },
      .now = []() { return std::chrono::system_clock::now(); },
      .active = [&active_states](bool active) {
        active_states.push_back(active);
      },
      .warning = [&warnings](const std::string &warning) {
        warnings.push_back(warning);
      },
  });

  run_until_terminal_log_stops(log, [&]() {
    elder_terms::set_terminal_log_connection_active(log, true);
    elder_terms::write_terminal_log(log, bytes("unwritten"), bytes("text"));
    elder_terms::set_terminal_log_connection_active(log, false);
  });
  elder_terms::destroy_terminal_log(log);

  expect_true(active_states.empty(),
              "failed log open should not activate the LOG indicator");
  expect_true(!warnings.empty() &&
                  warnings.front().find("failed to open terminal log") !=
                      std::string::npos,
              "failed log open should emit one actionable warning");
  expect_true(read_file(base_file) == "occupied",
              "failed log open should not alter the conflicting file");
  std::filesystem::remove_all(root);
}

} // namespace elder_terms_terminal_log_test

int main() {
  try {
    elder_terms_terminal_log_test::
        test_user_directory_base_paths_are_expanded();
    elder_terms_terminal_log_test::
        test_connection_name_path_placeholder_is_sanitized_once();
    elder_terms_terminal_log_test::
        test_connection_boundaries_reopen_formatted_paths_and_modes();
    elder_terms_terminal_log_test::
        test_settings_toggle_logging_while_connected();
    elder_terms_terminal_log_test::test_disabled_logging_creates_no_file();
    elder_terms_terminal_log_test::test_open_failure_warns_and_stops_safely();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
