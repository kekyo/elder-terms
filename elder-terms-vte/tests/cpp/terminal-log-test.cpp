#include "../../src/terminal-log.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace elder_terms_terminal_log_test {

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
      .base_directory = root.string(),
      .file_name_format = "{YYYYMMDD}/{hhmmss}_{fff}.txt",
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
  expect_true(read_file(root / "20260720" / "123456_123.txt") ==
                  "raw-first",
              "raw mode should preserve backend bytes in the first log");
  expect_true(read_file(root / "20260720" / "123501_456.txt") ==
                  "cooked-second",
              "cooked mode should write converted text in the reopened log");

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
        test_connection_boundaries_reopen_formatted_paths_and_modes();
    elder_terms_terminal_log_test::test_disabled_logging_creates_no_file();
    elder_terms_terminal_log_test::test_open_failure_warns_and_stops_safely();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
