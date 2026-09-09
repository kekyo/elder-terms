#include "../../src/ftp/curl-ftp-session.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace elder_terms_curl_ftp_session_test {

enum class Case { normal, cancel_final, cancel_paused, callback_failure,
                  quit_without_response };

static void expect(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void send_text(int fd, const std::string &text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count = ::send(fd, text.data() + offset,
                                 text.size() - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    expect(count > 0, "test server send failed");
    offset += static_cast<std::size_t>(count);
  }
}

static std::string read_line(int fd) {
  std::string result;
  char ch = 0;
  while (::read(fd, &ch, 1) == 1) {
    result += ch;
    if (result.ends_with("\r\n")) {
      result.resize(result.size() - 2);
      return result;
    }
  }
  return {};
}

static std::pair<int, unsigned> listen_local() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  expect(fd >= 0, "test socket failed");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                 0 &&
             ::listen(fd, 4) == 0,
         "test listener failed");
  socklen_t length = sizeof(address);
  expect(::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) ==
             0,
         "test listener address failed");
  return {fd, ntohs(address.sin_port)};
}

// The separate synchronization socket makes a delayed final response depend on
// an actual event processed by the caller, rather than a guessed sleep interval.
static void serve(int listener, int sync) {
  const int control = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
  expect(control >= 0, "test accept failed");
  ::close(listener);
  send_text(control, "220 Test ready\r\n");
  int data_listener = -1;
  for (;;) {
    const std::string command = read_line(control);
    if (command.empty()) {
      break;
    }
    if (command == "USER alice") {
      send_text(control, "331 Password\r\n");
    } else if (command == "PASS secret") {
      send_text(control, "230 Logged in\r\n");
    } else if (command == "PWD") {
      send_text(control, "257 \"/\"\r\n");
    } else if (command.starts_with("CWD ") || command == "TYPE I") {
      send_text(control, "200 OK\r\n");
    } else if (command.starts_with("SIZE ")) {
      send_text(control, "213 7\r\n");
    } else if (command == "EPSV") {
      const auto [fd, port] = listen_local();
      data_listener = fd;
      send_text(control, "229 (|||" + std::to_string(port) + "|)\r\n");
    } else if (command.starts_with("RETR ")) {
      send_text(control, "150 Opening data\r\n");
      const int data = ::accept4(data_listener, nullptr, nullptr, SOCK_CLOEXEC);
      ::close(data_listener);
      data_listener = -1;
      send_text(data, "content");
      ::close(data);
      send_text(sync, "final\r\n");
      const auto action = read_line(sync);
      if (action == "release") {
        send_text(control, "226 Complete\r\n");
      } else {
        expect(action == "cancel", "unexpected synchronization command");
        // A canceled final-response wait can still send QUIT before closing.
        // Supply no response: completion must come from client cancellation.
        auto closing = read_line(control);
        if (closing == "QUIT") closing = read_line(control);
        expect(closing.empty(), "cancel should close control: " + closing);
        break;
      }
    } else if (command == "QUIT") {
      send_text(sync, "quit\r\n");
      const auto action = read_line(sync);
      if (action == "release") {
        send_text(control, "221 Bye\r\n");
      } else {
        expect(action == "cancel", "Unexpected QUIT synchronization command");
        expect(read_line(control).empty(), "Stopping must close an unanswered QUIT");
      }
      break;
    } else {
      throw std::runtime_error("unexpected test command: " + command);
    }
  }
  if (data_listener >= 0) {
    ::close(data_listener);
  }
  ::close(control);
  ::close(sync);
}

static cardio::promise<elder_terms::CurlFtpResult> observe_transfer_async(
    cardio::promise<elder_terms::CurlFtpResult> transfer,
    std::shared_ptr<cardio::promise_source<void>> paused) {
  try {
    auto result = co_await transfer;
    (void)paused->try_reject(std::runtime_error("Transfer completed before pause"));
    co_return result;
  } catch (...) {
    (void)paused->try_reject(std::current_exception());
    throw;
  }
}

static cardio::promise<void> run_client(unsigned port, int sync,
                                        Case test_case) {
  const bool normal_transfer = test_case == Case::normal ||
                               test_case == Case::quit_without_response;
  const auto caller = std::this_thread::get_id();
  auto session = co_await elder_terms::open_curl_ftp_session_async({
      .connection = {.address = "127.0.0.1", .port = static_cast<int>(port),
                     .username = "alice", .data_connection_mode =
                         elder_terms::FtpDataConnectionMode::passive,
                     .local_directory = {}, .remote_directory = "/"},
      .password = "secret",
  });
  cardio::cancellation_source cancellation;
  auto cancellation_started = std::chrono::steady_clock::now();
  std::string bytes;
  std::atomic_bool resume_allowed = test_case == Case::quit_without_response;
  auto paused = std::make_shared<cardio::promise_source<void>>();
  auto paused_task = paused->get_promise();
  auto received = std::make_shared<cardio::promise_source<void>>();
  auto received_task = received->get_promise();
  if (test_case == Case::quit_without_response) {
    (void)paused->try_resolve();
  }
  elder_terms::CurlFtpRequest request;
  request.url = "ftp://127.0.0.1:" + std::to_string(port) + "/file";
  request.receive = [&](std::span<const std::byte> chunk) {
    expect(std::this_thread::get_id() != caller,
           "curl must run outside the caller thread");
    if (!resume_allowed.load()) {
      (void)paused->try_resolve();
      if (test_case == Case::callback_failure) {
        throw std::runtime_error("test receive callback failed");
      }
      return static_cast<std::size_t>(CURL_WRITEFUNC_PAUSE);
    }
    bytes.append(reinterpret_cast<const char *>(chunk.data()), chunk.size());
    if (bytes.size() == 7) (void)received->try_resolve();
    return chunk.size();
  };
  auto transfer = observe_transfer_async(
      session->perform_async(std::move(request), cancellation.get_cancellation()), paused);
  std::exception_ptr failure;
  try {
    if (test_case != Case::quit_without_response) {
      co_await paused_task;
    }
    expect(std::this_thread::get_id() == caller,
           "continuations must return to the caller dispatcher");
    if (test_case == Case::cancel_paused) {
      (void)cancellation.cancel();
    } else if (test_case != Case::callback_failure &&
               test_case != Case::quit_without_response) {
      resume_allowed.store(true);
      session->resume();
    }
    co_await cardio::from_fd(sync, cardio::fd_event::read);
    expect(read_line(sync) == "final", "server must await final response");
    if (test_case == Case::cancel_final) {
      co_await received_task;
      cancellation_started = std::chrono::steady_clock::now();
      (void)cancellation.cancel();
    }
    if (!normal_transfer) {
      send_text(sync, "cancel\r\n");
    } else {
      send_text(sync, "release\r\n");
    }
    bool cancelled = false;
    bool callback_failed = false;
    try {
      const auto result = co_await transfer;
      expect(result.code == CURLE_OK && result.response_code == 226,
             "transfer must include a successful final response");
    } catch (const cardio::canceled_exception &) {
      cancelled = true;
    } catch (const std::runtime_error &error) {
      callback_failed = std::string_view(error.what()) ==
                        "test receive callback failed";
      if (!callback_failed) throw;
    }
    expect(cancelled == (test_case == Case::cancel_final ||
                          test_case == Case::cancel_paused),
           "unexpected transfer cancellation");
    expect(callback_failed == (test_case == Case::callback_failure),
           "callback failure must cross the asynchronous boundary");
    if (test_case == Case::cancel_final) {
      expect(std::chrono::steady_clock::now() - cancellation_started <
                 std::chrono::seconds(10),
             "Cancellation must not await the normal FTP response timeout");
    }
    if (normal_transfer) {
      expect(bytes == "content", "pause/resume must deliver bytes once; received " +
                 std::to_string(bytes.size()) + " bytes: '" + bytes + "'");
    }
  } catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    // Release server-side gates even when an earlier assertion failed.
    (void)::shutdown(sync, SHUT_RDWR);
  }
  const auto stop_started = std::chrono::steady_clock::now();
  auto stopped = session->stop_async();
  try {
    if (normal_transfer && failure == nullptr) {
      co_await cardio::from_fd(sync, cardio::fd_event::read);
      expect(read_line(sync) == "quit", "cleanup should perform FTP QUIT");
      send_text(sync, test_case == Case::quit_without_response
                          ? "cancel\r\n" : "release\r\n");
    }
  } catch (...) {
    failure = std::current_exception();
  }
  co_await stopped;
  if (test_case == Case::quit_without_response) {
    // This case checks the shutdown deadline itself. Allow ample scheduling
    // overhead beyond its one-second grace period, including under emulation.
    expect(std::chrono::steady_clock::now() - stop_started <
               std::chrono::seconds(10),
           "Stopping must not await the normal FTP response timeout");
  }
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

static cardio::promise<void> execute_case_async(
    unsigned port, int sync, Case test_case, std::exception_ptr &failure,
    cardio::dispatcher_group_glib &group) {
  try {
    co_await run_client(port, sync, test_case);
  } catch (...) {
    failure = std::current_exception();
  }
  group.shutdown();
}

static void run_case(Case test_case) {
  static constexpr std::array names{
      "receive", "cancel final response", "cancel paused callback",
      "callback failure", "unanswered QUIT"};
  std::cout << "curl-ftp-session-test: "
            << names[static_cast<std::size_t>(test_case)] << std::endl;
  const auto [listener, port] = listen_local();
  int sync[2];
  expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sync) == 0,
         "synchronization socket failed");
  const pid_t server = ::fork();
  expect(server >= 0, "test fork failed");
  if (server == 0) {
    ::close(sync[0]);
    try {
      serve(listener, sync[1]);
      _exit(0);
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      _exit(1);
    }
  }
  ::close(listener);
  ::close(sync[1]);
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    auto task = execute_case_async(port, sync[0], test_case, failure, group);
    dispatcher.park();
  }
  ::close(sync[0]);
  if (failure != nullptr) {
    ::kill(server, SIGKILL);
  }
  int status = 0;
  while (::waitpid(server, &status, 0) < 0 && errno == EINTR) {
  }
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "FTP test server failed");
}

} // namespace elder_terms_curl_ftp_session_test

int main(int argc, char **argv) {
  try {
    using namespace elder_terms_curl_ftp_session_test;
    expect(argc == 2, "Expected a session test case name");
    const std::string name = argv[1];
    if (name == "receive") run_case(Case::normal);
    else if (name == "cancel-final") run_case(Case::cancel_final);
    else if (name == "cancel-paused") run_case(Case::cancel_paused);
    else if (name == "callback-failure") run_case(Case::callback_failure);
    else if (name == "unanswered-quit") run_case(Case::quit_without_response);
    else throw std::runtime_error("Unknown session test case: " + name);
    std::cout << "curl-ftp-session-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "curl-ftp-session-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
