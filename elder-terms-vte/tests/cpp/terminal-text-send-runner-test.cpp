#include "../../src/terminal-text-send-runner.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gio/gio.h>

namespace elder_terms {

struct FakeTextTransportState {
  std::uint64_t now_us = 0;
  std::uint64_t send_duration_us = 0;
  std::vector<std::uint64_t> send_times_us;
  std::vector<std::uint64_t> delays_us;
  std::vector<std::vector<unsigned char>> chunks;
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::filesystem::path temporary_file_path(const std::string &name) {
  return std::filesystem::temp_directory_path() /
         ("elder-terms-text-send-" + std::to_string(::getpid()) + "-" +
          std::to_string(static_cast<long long>(g_get_monotonic_time())) +
          "-" + name);
}

static std::string file_uri(const std::filesystem::path &path) {
  GFile *file = g_file_new_for_path(path.c_str());
  char *uri = g_file_get_uri(file);
  const std::string result = uri == nullptr ? std::string() : uri;
  g_free(uri);
  g_object_unref(file);
  return result;
}

static void write_file(const std::filesystem::path &path,
                       std::span<const unsigned char> bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output.good()) {
    throw std::runtime_error("failed to write text send test file");
  }
}

static TerminalTextSendTransport
make_transport(FakeTextTransportState *state) {
  return TerminalTextSendTransport{
      .send =
          [state](std::span<const unsigned char> bytes,
                  cardio::cancellation cancellation)
          -> cardio::promise<void> {
        cancellation.throw_if_cancellation_requested();
        state->send_times_us.push_back(state->now_us);
        state->chunks.emplace_back(bytes.begin(), bytes.end());
        state->now_us += state->send_duration_us;
        co_return;
      },
      .now_us = [state]() { return state->now_us; },
      .delay =
          [state](std::uint64_t delay_us,
                  cardio::cancellation cancellation)
          -> cardio::promise<void> {
        cancellation.throw_if_cancellation_requested();
        state->delays_us.push_back(delay_us);
        state->now_us += delay_us;
        co_return;
      },
  };
}

static void run_text_send(TerminalTextSendRequest request,
                          TerminalTextSendTransport transport) {
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  std::exception_ptr error;
  cardio::cancellation_source cancellation;
  auto root = [&]() -> cardio::promise<void> {
    try {
      co_await run_terminal_text_send_async(
          std::move(request), std::move(transport),
          cancellation.get_cancellation());
    } catch (...) {
      error = std::current_exception();
    }
    dispatcher_group.shutdown();
  }();
  dispatcher.park();
  root.unsafe_result();
  if (error) {
    std::rethrow_exception(error);
  }
}

static TerminalTextSendRequest
make_request(const std::filesystem::path &path, const std::string &encoding,
             std::uint64_t bytes_per_second) {
  return TerminalTextSendRequest{
      .source =
          TerminalTextSendFileSource{
              .uri = file_uri(path),
          },
      .text_settings =
          TerminalTextSettings{
              .encoding = encoding,
              .backspace_code = TerminalBackspaceCode::del,
              .cursor_key_mode = TerminalCursorKeyMode::trs80,
          },
      .bytes_per_second = bytes_per_second,
      .active = nullptr,
      .status = nullptr,
      .progress = nullptr,
      .finished = nullptr,
  };
}

static TerminalTextSendRequest
make_buffer_request(std::string utf8_text, const std::string &encoding,
                    std::uint64_t bytes_per_second) {
  return TerminalTextSendRequest{
      .source =
          TerminalTextSendBufferSource{
              .utf8_text = std::move(utf8_text),
          },
      .text_settings =
          TerminalTextSettings{
              .encoding = encoding,
              .backspace_code = TerminalBackspaceCode::del,
              .cursor_key_mode = TerminalCursorKeyMode::trs80,
          },
      .bytes_per_second = bytes_per_second,
      .active = nullptr,
      .status = nullptr,
      .progress = nullptr,
      .finished = nullptr,
  };
}

static std::vector<unsigned char>
join_chunks(const std::vector<std::vector<unsigned char>> &chunks) {
  std::vector<unsigned char> result;
  for (const auto &chunk : chunks) {
    result.insert(result.end(), chunk.begin(), chunk.end());
  }
  return result;
}

static std::vector<unsigned char> bytes_from_text(const std::string &text) {
  return std::vector<unsigned char>(text.begin(), text.end());
}

static void configure_newline_behavior(TerminalTextSendRequest *request,
                                       TerminalReturnCode return_code,
                                       bool follow_return_code) {
  request->text_settings.return_code = return_code;
  request->follow_return_code = follow_return_code;
}

static void throttles_encoded_payload_in_bounded_chunks() {
  const std::filesystem::path path = temporary_file_path("throttle.txt");
  const std::vector<unsigned char> input{'a', 'b', 'c', 'd', 'e'};
  write_file(path, input);

  FakeTextTransportState state;
  run_text_send(make_request(path, "UTF-8", 20), make_transport(&state));
  std::filesystem::remove(path);

  expect_true(state.chunks ==
                  std::vector<std::vector<unsigned char>>{
                      {'a', 'b'}, {'c', 'd'}, {'e'}},
              "20 bytes/s should send at most two bytes per 100ms chunk");
  expect_true(state.send_times_us ==
                  std::vector<std::uint64_t>{0, 100000, 200000},
              "encoded chunks should follow the configured byte rate");
  expect_true(state.delays_us ==
                  std::vector<std::uint64_t>{100000, 100000},
              "the runner should asynchronously delay between chunks");
}

static void backend_backpressure_satisfies_the_rate_delay() {
  const std::filesystem::path path = temporary_file_path("backpressure.txt");
  const std::vector<unsigned char> input{'a', 'b', 'c', 'd'};
  write_file(path, input);

  FakeTextTransportState state;
  state.send_duration_us = 200000;
  run_text_send(make_request(path, "UTF-8", 20), make_transport(&state));
  std::filesystem::remove(path);

  expect_true(state.send_times_us ==
                  std::vector<std::uint64_t>{0, 200000},
              "the runner should await each backend write");
  expect_true(state.delays_us.empty(),
              "slow backend writes should satisfy the throttle interval");
}

static void encodes_text_and_reports_source_progress_and_replacements() {
  const std::filesystem::path path = temporary_file_path("encoding.txt");
  const std::vector<unsigned char> input{
      0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0x1b, '[', 'A',
      0xf0, 0x9f, 0x98, 0x80,
  };
  write_file(path, input);

  FakeTextTransportState state;
  std::vector<std::string> statuses;
  std::vector<TerminalTransferProgress> progress;
  TerminalTextSendRequest request = make_request(path, "SHIFT-JIS", 8000000);
  request.status = [&statuses](const std::string &status) {
    statuses.push_back(status);
  };
  request.progress = [&progress](TerminalTransferProgress update) {
    progress.push_back(update);
  };
  run_text_send(std::move(request), make_transport(&state));
  std::filesystem::remove(path);

  expect_true(join_chunks(state.chunks) ==
                  std::vector<unsigned char>{
                      0x93, 0xfa, 0x96, 0x7b, 0x1b, '[', 'A', '?'},
              "text send should encode UTF-8 without terminal key mapping");
  expect_true(std::count(statuses.begin(), statuses.end(),
                         "Text contained characters that were replaced") == 1,
              "lossy text conversion should report one warning");
  expect_true(!progress.empty() &&
                  progress.back().mode ==
                      TerminalTransferProgressMode::determinate &&
                  progress.back().fraction.has_value() &&
                  *progress.back().fraction == 1.0,
              "text send progress should finish at the source file size");
}

static void
encodes_and_throttles_buffered_text_with_progress_and_replacements() {
  FakeTextTransportState state;
  std::vector<std::string> statuses;
  std::vector<TerminalTransferProgress> progress;
  TerminalTextSendRequest request =
      make_buffer_request("日本😀01234", "SHIFT-JIS", 20);
  request.status = [&statuses](const std::string &status) {
    statuses.push_back(status);
  };
  request.progress = [&progress](TerminalTransferProgress update) {
    progress.push_back(update);
  };

  run_text_send(std::move(request), make_transport(&state));

  expect_true(join_chunks(state.chunks) ==
                  std::vector<unsigned char>{
                      0x93, 0xfa, 0x96, 0x7b, '?',
                      '0',  '1',  '2',  '3',  '4'},
              "buffered text should use the configured terminal encoding");
  expect_true(state.send_times_us ==
                  std::vector<std::uint64_t>{
                      0, 100000, 200000, 300000, 400000},
              "buffered text should use the configured byte rate");
  expect_true(state.delays_us ==
                  std::vector<std::uint64_t>{
                      100000, 100000, 100000, 100000},
              "buffered text should asynchronously delay between chunks");
  expect_true(std::count(statuses.begin(), statuses.end(),
                         "Text contained characters that were replaced") == 1,
              "buffered lossy conversion should report one warning");
  expect_true(!progress.empty() &&
                  progress.back().mode ==
                      TerminalTransferProgressMode::determinate &&
                  progress.back().fraction.has_value() &&
                  *progress.back().fraction == 1.0,
              "buffered text progress should finish at its UTF-8 byte size");
}

static void normalizes_file_and_buffer_line_endings_to_the_return_code() {
  const std::string input = "a\r\nb\rc\nd";
  struct NewlineCase {
    TerminalReturnCode return_code;
    const char *expected;
    const char *name;
  };
  const NewlineCase cases[] = {
      {.return_code = TerminalReturnCode::automatic,
       .expected = "a\rb\rc\rd",
       .name = "Auto"},
      {.return_code = TerminalReturnCode::cr,
       .expected = "a\rb\rc\rd",
       .name = "CR"},
      {.return_code = TerminalReturnCode::lf,
       .expected = "a\nb\nc\nd",
       .name = "LF"},
      {.return_code = TerminalReturnCode::crlf,
       .expected = "a\r\nb\r\nc\r\nd",
       .name = "CRLF"},
  };

  const std::filesystem::path path = temporary_file_path("newlines.txt");
  write_file(path, bytes_from_text(input));
  for (const NewlineCase &test_case : cases) {
    FakeTextTransportState file_state;
    TerminalTextSendRequest file_request =
        make_request(path, "UTF-8", 8000000);
    configure_newline_behavior(&file_request, test_case.return_code, true);
    run_text_send(std::move(file_request), make_transport(&file_state));
    expect_true(join_chunks(file_state.chunks) ==
                    bytes_from_text(test_case.expected),
                std::string(test_case.name) +
                    " should normalize file line endings");

    FakeTextTransportState buffer_state;
    TerminalTextSendRequest buffer_request =
        make_buffer_request(input, "UTF-8", 8000000);
    configure_newline_behavior(&buffer_request, test_case.return_code, true);
    run_text_send(std::move(buffer_request), make_transport(&buffer_state));
    expect_true(join_chunks(buffer_state.chunks) ==
                    bytes_from_text(test_case.expected),
                std::string(test_case.name) +
                    " should normalize buffered line endings");
  }
  std::filesystem::remove(path);
}

static void preserves_line_endings_when_return_code_following_is_disabled() {
  const std::string input = "a\r\nb\rc\nd\r";
  FakeTextTransportState state;
  TerminalTextSendRequest request =
      make_buffer_request(input, "UTF-8", 8000000);
  configure_newline_behavior(&request, TerminalReturnCode::lf, false);

  run_text_send(std::move(request), make_transport(&state));

  expect_true(join_chunks(state.chunks) == bytes_from_text(input),
              "disabled Return-code following should preserve source bytes");
}

static void preserves_crlf_across_file_read_boundaries_and_flushes_eof_cr() {
  std::vector<unsigned char> input(65535, 'a');
  input.push_back('\r');
  input.push_back('\n');
  input.push_back('b');
  input.push_back('\r');
  const std::filesystem::path path =
      temporary_file_path("boundary-newlines.txt");
  write_file(path, input);

  FakeTextTransportState state;
  std::vector<TerminalTransferProgress> progress;
  TerminalTextSendRequest request = make_request(path, "UTF-8", 8000000);
  configure_newline_behavior(&request, TerminalReturnCode::crlf, true);
  request.progress = [&progress](TerminalTransferProgress update) {
    progress.push_back(update);
  };
  run_text_send(std::move(request), make_transport(&state));
  std::filesystem::remove(path);

  std::vector<unsigned char> expected(65535, 'a');
  expected.insert(expected.end(), {'\r', '\n', 'b', '\r', '\n'});
  expect_true(join_chunks(state.chunks) == expected,
              "split CRLF should stay one newline and EOF CR should flush");
  const double expected_first_fraction =
      65536.0 / static_cast<double>(input.size());
  expect_true(progress.size() >= 3 && progress[1].fraction.has_value() &&
                  *progress[1].fraction == expected_first_fraction,
              "progress should count consumed source bytes at read boundaries");
}

} // namespace elder_terms

int main() {
  elder_terms::throttles_encoded_payload_in_bounded_chunks();
  elder_terms::backend_backpressure_satisfies_the_rate_delay();
  elder_terms::encodes_text_and_reports_source_progress_and_replacements();
  elder_terms::
      encodes_and_throttles_buffered_text_with_progress_and_replacements();
  elder_terms::normalizes_file_and_buffer_line_endings_to_the_return_code();
  elder_terms::preserves_line_endings_when_return_code_following_is_disabled();
  elder_terms::preserves_crlf_across_file_read_boundaries_and_flushes_eof_cr();
  return 0;
}
