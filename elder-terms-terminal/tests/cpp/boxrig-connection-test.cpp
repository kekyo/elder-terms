// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-connection.h"

#include <glib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

struct HandoffWireHeader {
  std::uint32_t size;
  std::uint32_t child;
  std::uint32_t capacity;
  std::uint32_t chunk;
  std::uint8_t input_finished;
};

static void socket_bytes(int fd, void *data, std::size_t size, bool sending) {
  auto *cursor = static_cast<unsigned char *>(data);
  while (size) {
    const auto count = sending ? ::write(fd, cursor, size) : ::read(fd, cursor, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("handoff socket closed");
    cursor += count;
    size -= static_cast<std::size_t>(count);
  }
}

static void socket_master(int socket, int *master, bool sending) {
  char tag = 'F';
  struct iovec payload{&tag, 1};
  union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control{};
  struct msghdr message{};
  message.msg_iov = &payload;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  if (sending) {
    auto *item = CMSG_FIRSTHDR(&message);
    item->cmsg_level = SOL_SOCKET;
    item->cmsg_type = SCM_RIGHTS;
    item->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(item), master, sizeof(int));
    if (::sendmsg(socket, &message, 0) != 1) throw std::runtime_error("send handoff master");
  } else {
    if (::recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || tag != 'F' ||
        (message.msg_flags & MSG_CTRUNC)) throw std::runtime_error("receive handoff master");
    const auto *item = CMSG_FIRSTHDR(&message);
    if (!item || item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
        item->cmsg_len != CMSG_LEN(sizeof(int))) throw std::runtime_error("invalid handoff master");
    std::memcpy(master, CMSG_DATA(item), sizeof(int));
  }
}

using namespace elder_terms;

struct Test {
  boxrig_terminal *core = nullptr;
  TerminalConnection *connection = nullptr;
  cardio::primitives::conditional changed;
  guint dispatch_source = 0;
  bool paused = false;
  std::string raw;
  std::string cooked;
  std::size_t titles = 0;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void wake(Test *test);

static gboolean dispatch(gpointer context) {
  auto *test = static_cast<Test *>(context);
  test->dispatch_source = 0;
  const auto progress = terminal_connection_dispatch(test->connection);
  boxrig_host_item item{};
  bool consumed = false;
  while (boxrig_terminal_host_item_peek(test->core, &item)) {
    if (item.type == BOXRIG_HOST_ITEM_TITLE) ++test->titles;
    const auto request = item.request_id;
    boxrig_terminal_host_item_consume(test->core);
    if (request) boxrig_terminal_complete_host_request(test->core, request, BOXRIG_HOST_REQUEST_UNAVAILABLE, nullptr, 0);
    consumed = true;
  }
  if (progress.dispatch_pending || consumed) wake(test);
  test->changed.trigger();
  return G_SOURCE_REMOVE;
}

static void wake(Test *test) {
  test->changed.trigger();
  if (!test->paused && !test->dispatch_source)
    test->dispatch_source = g_idle_add_full(G_PRIORITY_DEFAULT, dispatch, test, nullptr);
}

static std::string contents(Test *test) {
  const boxrig_text_snapshot_options options{BOXRIG_TEXT_SCREEN_PRIMARY, nullptr, 65536, 1024};
  boxrig_text_snapshot *snapshot = nullptr;
  expect(boxrig_terminal_create_text_snapshot(test->core, &options, &snapshot) == BOXRIG_STATUS_OK, "text snapshot");
  size_t length = 0;
  const auto *bytes = boxrig_text_snapshot_data(snapshot, &length);
  const std::string text(reinterpret_cast<const char *>(bytes), length);
  boxrig_text_snapshot_destroy(snapshot);
  return text;
}

static TerminalLocalCommand command(std::string script) {
  return {{"/bin/sh", "-c", std::move(script)}, "xterm-256color", 17, 3};
}

static cardio::promise<void> test_binary_transfer_chunks(Test *test) {
  const auto path = std::filesystem::temp_directory_path() /
      ("elder-terms-binary-transfer-" + std::to_string(::getpid()));
  std::vector<unsigned char> payload(256);
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<unsigned char>(index);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(payload.data()), payload.size());
    expect(static_cast<bool>(file), "write binary transfer source");
  }
  std::vector<unsigned char> incoming;
  std::vector<unsigned char> frame;
  std::vector<unsigned char> received;
  std::size_t write_count = 0;
  std::vector<cardio::cancellation> read_tokens;
  std::vector<cardio::cancellation> write_tokens;
  bool fail_write = false;
  cardio::primitives::conditional data_ready;
  TerminalTransportRequest transport;
  transport.encoding = "CP932";
  transport.io_chunk_size = 1;
  transport.open = [&](std::size_t, std::size_t, cardio::cancellation)
      -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
        [&](std::span<unsigned char> bytes, cardio::cancellation stop)
            -> cardio::promise<std::size_t> {
          read_tokens.push_back(stop);
          while (incoming.empty()) co_await data_ready.wait(stop);
          const auto count = std::min(bytes.size(), incoming.size());
          std::copy_n(incoming.begin(), count, bytes.begin());
          incoming.erase(incoming.begin(), incoming.begin() + count);
          co_return count;
        },
        [&](std::span<const unsigned char> bytes, cardio::cancellation stop)
            -> cardio::promise<void> {
          write_tokens.push_back(stop);
          if (fail_write) throw std::system_error(EIO, std::generic_category(), "test transfer write failure");
          ++write_count;
          frame.insert(frame.end(), bytes.begin(), bytes.end());
          for (;;) {
            if (frame.empty()) break;
            if (frame.front() == 4) {
              frame.erase(frame.begin());
            } else {
              if (frame.size() < 133) break;
              expect(frame[0] == 1 && frame[1] + frame[2] == 255,
                     "receiver should see a complete XMODEM CRC frame");
              received.insert(received.end(), frame.begin() + 3, frame.begin() + 131);
              frame.erase(frame.begin(), frame.begin() + 133);
            }
            incoming.push_back(6);
            data_ready.trigger();
          }
          co_return;
        },
        [](std::size_t, std::size_t, cardio::cancellation)
            -> cardio::promise<void> { co_return; },
        []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_connection_start_transport(test->connection, std::move(transport)),
         "start binary transfer peer");
  while (terminal_connection_state(test->connection).phase != TerminalConnectionPhase::connected)
    co_await test->changed.wait();
  expect(terminal_connection_prepare_transfer(test->connection), "reserve binary transfer");
  incoming.push_back('C');
  data_ready.trigger();
  TerminalTransferRequest request;
  request.protocol = TerminalTransferProtocol::xmodem;
  request.direction = TerminalTransferDirection::send;
  request.source_file_uris = {path.string()};
  request.options.xmodem_packet_size = TerminalTransferXmodemPacketSize::bytes_128;
  const auto succeeded = co_await terminal_connection_transfer(test->connection, request, {});
  expect(!write_tokens.empty() && std::all_of(write_tokens.begin(), write_tokens.end(),
      [](const cardio::cancellation &token) { return token.is_cancellation_requested(); }),
      "completed transfer writes must release their linked deadlines");
  expect(read_tokens.size() >= 2 && std::all_of(read_tokens.begin(), read_tokens.end() - 1,
      [](const cardio::cancellation &token) { return token.is_cancellation_requested(); }),
      "completed reads must release their linked cancellation");
  const auto writes_before_failure = write_tokens.size();
  fail_write = true;
  expect(terminal_connection_prepare_transfer(test->connection), "reserve failing binary transfer");
  incoming.push_back('C');
  data_ready.trigger();
  cardio::cancellation_source caller;
  const auto failed = co_await terminal_connection_transfer(test->connection, request, caller.get_cancellation());
  expect(!failed && write_tokens.size() > writes_before_failure,
         "transport write failure should fail the transfer");
  expect(std::all_of(write_tokens.begin() + writes_before_failure, write_tokens.end(),
      [](const cardio::cancellation &token) { return token.is_cancellation_requested(); }),
      "failed transfer writes must release their linked deadlines");
  expect(!caller.get_cancellation().is_cancellation_requested(),
         "releasing a transfer deadline must not cancel its caller");
  terminal_connection_stop(test->connection);
  co_await terminal_connection_wait_stopped(test->connection);
  std::filesystem::remove(path);
  expect(succeeded, "binary transfer should complete");
  expect(received == payload && frame.empty(),
         "binary transfer should preserve every byte independently of encoding");
  expect(write_count == 3,
         "text conversion chunk size must not split two binary frames and EOT");
}

static cardio::promise<void> scenarios(Test *test) {
  test->paused = true;
  expect(terminal_connection_start_local(test->connection, command(
      "stty raw -echo; i=0; while [ \"$i\" -lt 200 ]; do printf '\\033]2;x\\007\\033[6n'; i=$((i+1)); done; "
      "expected=$(i=0; while [ \"$i\" -lt 200 ]; do printf '\\033[1;1R'; i=$((i+1)); done); "
      "actual=$(dd bs=1 count=1200 iflag=fullblock 2>/dev/null); "
      "if [ \"$actual\" = \"$expected\" ]; then printf '\\r\\nSTRESS_OK\\r\\n\\342\\202'; else printf BAD_REPLY; exit 3; fi")), "start stress peer");
  while (terminal_connection_state(test->connection).receive_peak != 17) co_await test->changed.wait();
  expect(terminal_connection_state(test->connection).receive_peak == 17, "receive queue reaches its exact bound");
  test->paused = false;
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  auto state = terminal_connection_state(test->connection);
  expect(state.exit_code == 0 && state.reaped && state.operation_error == 0, "stress peer completes and is reaped");
  expect(state.receive_peak <= 17 && state.send_peak > 0 && state.send_peak <= 17, "both queues remain bounded");
  expect(test->raw.ends_with("\xe2\x82"), "raw output preserves the incomplete UTF-8 bytes");
  expect(test->cooked == test->raw.substr(0, test->raw.size() - 2) + "�",
      "cooked output includes the decoder's one EOF replacement");
  expect(test->titles == 200, "host backpressure does not duplicate or lose title events");
  expect(contents(test).find("STRESS_OK\n�") != std::string::npos, "EOF drains UTF-8 tail after all replies");
  expect(::kill(state.child, 0) == -1 && errno == ESRCH, "no zombie stress child");

  const auto previous_size = test->raw.size();
  expect(terminal_connection_start_local(test->connection, command("stty -echo; printf '\\342'; read value")), "start incomplete UTF-8 peer");
  while (test->raw.size() == previous_size) co_await test->changed.wait();
  terminal_connection_dispatch(test->connection);
  terminal_connection_stop(test->connection);
  co_await terminal_connection_wait_stopped(test->connection);
  expect(contents(test).find("��") != std::string::npos, "disconnect finalizes this generation's partial UTF-8");

  expect(terminal_connection_start_local(test->connection, command("printf '\\202\\254RECONNECTED'")), "reconnect after incomplete input");
  co_await terminal_connection_wait_stopped(test->connection);
  expect(contents(test).find("€") == std::string::npos, "old and new generation bytes never form one character");
  expect(contents(test).find("RECONNECTED") != std::string::npos, "core and display survive reconnection");

  test->paused = true;
  expect(terminal_connection_start_local(test->connection,
      command("stty -echo; printf 'CANCEL_QUEUE_TAIL'; read value")), "start queued cancellation peer");
  while (terminal_connection_state(test->connection).receive_peak != 17) co_await test->changed.wait();
  terminal_connection_stop(test->connection);
  test->paused = false;
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  expect(contents(test).find("CANCEL_QUEUE_TAIL") != std::string::npos, "cancellation preserves bytes already received");

  auto encoded = command(
      "stty raw -echo; printf 'ENCODE_READY'; "
      "expected=$(printf '\\223\\372\\226\\173\\214\\352\\223\\372\\226\\173\\214\\352'); "
      "actual=$(dd bs=1 count=12 iflag=fullblock 2>/dev/null); "
      "if [ \"$actual\" != \"$expected\" ]; then printf BAD_ENCODING; exit 3; fi; "
      "printf '\\r\\n\\223\\372\\226\\173\\214\\352\\202'");
  encoded.encoding = "CP932";
  encoded.queue_capacity = 1;
  encoded.io_chunk_size = 1;
  const auto encoded_start = test->cooked.size();
  expect(terminal_connection_start_local(test->connection, std::move(encoded)), "start encoded stream");
  while (test->cooked.find("ENCODE_READY", encoded_start) == std::string::npos) co_await test->changed.wait();
  const std::string text = "日本語日本語";
  expect(boxrig_terminal_send_text(test->core, text.data(), text.size()) == BOXRIG_STATUS_OK, "queue UTF-8 text");
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  state = terminal_connection_state(test->connection);
  expect(state.exit_code == 0 && state.reaped, "CP932 peer receives each encoded byte exactly once");
  expect(test->cooked.substr(encoded_start) == "ENCODE_READY\r\n日本語�", "split CP932 input and EOF are converted exactly once");
  expect(state.receive_peak == 1 && state.send_peak == 1, "converted characters span single-byte bounded queues");

  test->paused = true;
  auto cancelled_encoding = command("stty -echo; printf '\\223\\372'; read value");
  cancelled_encoding.encoding = "CP932";
  cancelled_encoding.queue_capacity = 1;
  cancelled_encoding.io_chunk_size = 1;
  const auto cancelled_start = test->cooked.size();
  expect(terminal_connection_start_local(test->connection, std::move(cancelled_encoding)), "start converted cancellation peer");
  while (terminal_connection_state(test->connection).receive_peak != 1) co_await test->changed.wait();
  terminal_connection_stop(test->connection);
  test->paused = false;
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  expect(test->cooked.substr(cancelled_start) == "日", "cancellation reports the converted chunk only once");
  expect(contents(test).find("日", contents(test).find("日本語�") + std::string("日本語�").size()) != std::string::npos,
      "cancellation preserves the converted remainder beyond queue capacity");

  auto invalid_encoding = command("printf SHOULD_NOT_SPAWN");
  invalid_encoding.encoding = "ELDER_TERMS_INVALID_ENCODING";
  expect(terminal_connection_start_local(test->connection, std::move(invalid_encoding)), "accept an observable conversion failure");
  co_await terminal_connection_wait_stopped(test->connection);
  state = terminal_connection_state(test->connection);
  expect(state.operation_error == EINVAL && state.child == 0, "invalid conversion fails before spawning a child");

  std::vector<unsigned char> live_input{0x93, 0xfa, 0x82};
  std::string live_output;
  cardio::primitives::conditional live_received;
  cardio::primitives::conditional release_write;
  bool live_eof = false;
  bool first_write = true;
  TerminalTransportRequest live;
  live.encoding = "CP932";
  live.queue_capacity = 1;
  live.io_chunk_size = 1;
  live.open = [&](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
        [&](std::span<unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<std::size_t> {
          while (live_input.empty() && !live_eof) co_await live_received.wait(stop);
          if (live_input.empty()) co_return 0;
          bytes[0] = live_input.front();
          live_input.erase(live_input.begin());
          co_return 1;
        },
        [&](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
          live_output.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
          test->changed.trigger();
          if (std::exchange(first_write, false)) co_await release_write.wait(stop);
        },
        [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
        []() -> cardio::promise<void> { co_return; }};
  };
  const auto live_raw_start = test->raw.size();
  const auto live_cooked_start = test->cooked.size();
  expect(terminal_connection_start_transport(test->connection, std::move(live)), "start live encoding peer");
  while (test->raw.size() < live_raw_start + 3) co_await test->changed.wait();
  const auto live_generation = terminal_connection_state(test->connection).generation;
  expect(boxrig_terminal_send_text(test->core, "日", 3) == BOXRIG_STATUS_OK, "queue old-encoding text");
  wake(test);
  while (live_output.empty()) co_await test->changed.wait();
  auto exchange_encoding = terminal_connection_change_encoding(test->connection, "UTF-8");
  expect(terminal_connection_state(test->connection).encoding == "CP932", "encoding exchange waits for an in-flight write");
  release_write.trigger();
  expect(co_await exchange_encoding, "encoding exchange completes after the old write");
  expect(live_output == std::string("\x93\xfa", 2), "old output is completely encoded before exchange");
  expect(terminal_connection_state(test->connection).generation == live_generation, "encoding exchange preserves the connection generation");
  expect(!(co_await terminal_connection_change_encoding(test->connection, "ELDER_TERMS_INVALID_ENCODING")), "invalid live encoding is rejected");
  expect(terminal_connection_state(test->connection).encoding == "UTF-8", "rejected encoding leaves the old converters usable");
  expect(boxrig_terminal_send_text(test->core, "語", 3) == BOXRIG_STATUS_OK, "queue new-encoding text");
  wake(test);
  while (live_output.size() != 5) co_await test->changed.wait();
  expect(live_output == std::string("\x93\xfa", 2) + "語", "later output uses the new encoding exactly once");
  const std::string live_tail = "語";
  live_input.assign(live_tail.begin(), live_tail.end());
  live_eof = true;
  live_received.trigger();
  co_await terminal_connection_wait_stopped(test->connection);
  expect(test->cooked.substr(live_cooked_start) == "日�語", "live exchange flushes the old partial input before the new stream");

  const auto cancelled_exchange_start = test->cooked.size();
  bool cancelling_write_started = false;
  TerminalTransportRequest cancelling_exchange;
  cancelling_exchange.encoding = "ISO-2022-JP";
  cancelling_exchange.open = [&](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
        [&](std::span<unsigned char>, cardio::cancellation stop) -> cardio::promise<std::size_t> {
          co_await live_received.wait(stop);
          co_return 0;
        },
        [&](std::span<const unsigned char>, cardio::cancellation stop) -> cardio::promise<void> {
          cancelling_write_started = true;
          test->changed.trigger();
          co_await release_write.wait(stop);
        },
        [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
        []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_connection_start_transport(test->connection, std::move(cancelling_exchange)), "start cancelled encoding peer");
  while (terminal_connection_state(test->connection).phase != TerminalConnectionPhase::connected) co_await test->changed.wait();
  expect(boxrig_terminal_send_text(test->core, "日", 3) == BOXRIG_STATUS_OK, "queue text before cancelling encoding exchange");
  wake(test);
  while (!cancelling_write_started) co_await test->changed.wait();
  auto cancelled_exchange = terminal_connection_change_encoding(test->connection, "UTF-8");
  terminal_connection_stop(test->connection);
  expect(!(co_await cancelled_exchange), "disconnect cancels an encoding exchange waiting for output");
  co_await terminal_connection_wait_stopped(test->connection);
  expect(test->cooked.size() == cancelled_exchange_start, "cancelled empty input has no duplicate finalization");

  co_await test_binary_transfer_chunks(test);

  for (int attempt = 0; attempt < 4; ++attempt) {
    expect(terminal_connection_start_local(test->connection, command("trap '' HUP TERM; read value")), "start cancellable child");
    terminal_connection_stop(test->connection);
    expect(!terminal_connection_start_local(test->connection, command("exit 0")), "reconnect waits for recovery");
    co_await terminal_connection_wait_stopped(test->connection);
    state = terminal_connection_state(test->connection);
    expect(state.cancelled && state.reaped && state.operation_error == 0, "startup cancellation recovers the child");
    expect(::kill(state.child, 0) == -1 && errno == ESRCH, "cancelled child cannot remain a zombie");
  }

  expect(terminal_connection_start_local(test->connection, {}), "parent diagnostic is observable");
  co_await terminal_connection_wait_stopped(test->connection);
  state = terminal_connection_state(test->connection);
  expect(state.spawn.state == BOXRIG_POSIX_PTY_SPAWN_FAILED && state.spawn.stage == BOXRIG_POSIX_PTY_STAGE_ARGUMENTS &&
      state.spawn.error_number == EINVAL && state.child == 0 && !state.reaped, "parent preparation failure is separate from process exit");

  expect(terminal_connection_start_local(test->connection, command(
      "stty -echo; echo HANDOFF_READY; while read value; do echo \"$value\"; "
      "[ \"$value\" = quit ] && exit 0; done")), "start handoff shell");
  while (contents(test).find("HANDOFF_READY") == std::string::npos) co_await test->changed.wait();
  auto rolled_back = co_await terminal_connection_prepare_local_handoff(test->connection);
  expect(rolled_back.has_value() && rolled_back->master_fd >= 0, "prepare local checkpoint and master");
  const auto original_child = rolled_back->child;
  ::close(rolled_back->master_fd);
  terminal_connection_rollback_local_handoff(test->connection);
  expect(boxrig_terminal_send_text(test->core, "ROLLBACK\n", 9) == BOXRIG_STATUS_OK,
      "send after handoff rollback");
  wake(test);
  while (contents(test).find("ROLLBACK") == std::string::npos) co_await test->changed.wait();
  auto handoff = co_await terminal_connection_prepare_local_handoff(test->connection);
  expect(handoff.has_value() && handoff->child == original_child, "prepare same shell after rollback");

  Test destination;
  expect(boxrig_terminal_import_checkpoint(handoff->checkpoint.data(), handoff->checkpoint.size(),
      64 * 1024 * 1024, &destination.core) == BOXRIG_STATUS_OK, "restore terminal checkpoint");
  destination.connection = create_terminal_connection(destination.core,
      [&destination] { wake(&destination); },
      [&destination](std::span<const unsigned char>, std::span<const unsigned char>) {
        destination.changed.trigger();
      });
  expect(terminal_connection_resume_local(destination.connection, handoff->master_fd,
      handoff->input_finished, handoff->child, handoff->queue_capacity, handoff->io_chunk_size),
      "resume direct PTY I/O");
  handoff->master_fd = -1;
  auto *reaper = terminal_connection_commit_local_handoff(test->connection);
  expect(reaper != nullptr, "commit supplies an independent child reaper");
  co_await terminal_connection_wait_stopped(test->connection);
  expect(terminal_connection_state(test->connection).phase == TerminalConnectionPhase::closed,
      "source connection closes while transferred shell is alive");
  destroy_terminal_connection(test->connection);
  test->connection = create_terminal_connection(test->core, [&] { wake(test); },
      [&](std::span<const unsigned char>, std::span<const unsigned char>) {});
  expect(boxrig_terminal_send_text(destination.core, "AFTER_HANDOFF\n", 14) == BOXRIG_STATUS_OK,
      "send to transferred shell");
  wake(&destination);
  while (contents(&destination).find("AFTER_HANDOFF") == std::string::npos)
    co_await destination.changed.wait();
  auto second_handoff = co_await terminal_connection_prepare_local_handoff(destination.connection);
  expect(second_handoff.has_value() && second_handoff->child == original_child,
      "export a previously imported PTY session again");
  Test third;
  expect(boxrig_terminal_import_checkpoint(second_handoff->checkpoint.data(),
      second_handoff->checkpoint.size(), 64 * 1024 * 1024, &third.core) == BOXRIG_STATUS_OK,
      "restore second-generation terminal checkpoint");
  third.connection = create_terminal_connection(third.core, [&third] { wake(&third); },
      [&third](std::span<const unsigned char>, std::span<const unsigned char>) {
        third.changed.trigger();
      });
  expect(terminal_connection_resume_local(third.connection, second_handoff->master_fd,
      second_handoff->input_finished, second_handoff->child,
      second_handoff->queue_capacity, second_handoff->io_chunk_size),
      "adopt second-generation PTY master");
  second_handoff->master_fd = -1;
  expect(terminal_connection_commit_local_handoff(destination.connection) == nullptr,
      "a relay generation cannot reap the original owner's child");
  co_await terminal_connection_wait_stopped(destination.connection);
  destroy_terminal_connection(destination.connection);
  boxrig_terminal_destroy(destination.core);
  expect(boxrig_terminal_send_text(third.core, "quit\n", 5) == BOXRIG_STATUS_OK,
      "stop twice-transferred shell");
  wake(&third);
  co_await terminal_connection_wait_stopped(third.connection);
  const auto exit = co_await terminal_local_child_reaper_wait(reaper);
  expect(exit.child == original_child && exit.exit_code == 0 && exit.error == 0,
      "independent original owner reaps the transferred child exactly once");
  destroy_terminal_local_child_reaper(reaper);
  destroy_terminal_connection(third.connection);
  boxrig_terminal_destroy(third.core);

  expect(terminal_connection_start_local(test->connection, command(
      "stty -echo; echo PROCESS_READY; while read value; do echo \"$$ $value\"; "
      "[ \"$value\" = quit ] && exit 0; done")), "start interprocess shell");
  while (contents(test).find("PROCESS_READY") == std::string::npos) co_await test->changed.wait();
  auto process_handoff = co_await terminal_connection_prepare_local_handoff(test->connection);
  expect(process_handoff.has_value(), "prepare interprocess handoff");
  int channel[2]{};
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, channel) == 0, "create handoff socket");
  const auto receiver = ::fork();
  expect(receiver >= 0, "fork destination process");
  if (receiver == 0) {
    ::close(channel[0]);
    const auto descriptor = std::to_string(channel[1]);
    char *arguments[] = {const_cast<char *>("boxrig-connection-test"),
        const_cast<char *>("--handoff-receiver"), const_cast<char *>(descriptor.c_str()), nullptr};
    ::execv("/proc/self/exe", arguments);
    _exit(127);
  }
  ::close(channel[1]);
  socket_master(channel[0], &process_handoff->master_fd, true);
  ::close(process_handoff->master_fd);
  HandoffWireHeader wire{static_cast<std::uint32_t>(process_handoff->checkpoint.size()),
      static_cast<std::uint32_t>(process_handoff->child),
      static_cast<std::uint32_t>(process_handoff->queue_capacity),
      static_cast<std::uint32_t>(process_handoff->io_chunk_size),
      static_cast<std::uint8_t>(process_handoff->input_finished)};
  socket_bytes(channel[0], &wire, sizeof(wire), true);
  socket_bytes(channel[0], process_handoff->checkpoint.data(), process_handoff->checkpoint.size(), true);
  char ready = 0;
  socket_bytes(channel[0], &ready, 1, false);
  expect(ready == 'R', "destination restored transferred terminal");
  auto *process_reaper = terminal_connection_commit_local_handoff(test->connection);
  expect(process_reaper != nullptr, "commit interprocess transfer");
  co_await terminal_connection_wait_stopped(test->connection);
  destroy_terminal_connection(test->connection);
  test->connection = create_terminal_connection(test->core, [&] { wake(test); },
      [&](std::span<const unsigned char>, std::span<const unsigned char>) {});
  char commit = 'C';
  socket_bytes(channel[0], &commit, 1, true);
  const auto receiver_status = co_await cardio::promises::start_new([receiver] {
    int status = 0;
    if (::waitpid(receiver, &status, 0) != receiver) throw std::runtime_error("wait destination");
    return status;
  });
  const auto process_exit = co_await terminal_local_child_reaper_wait(process_reaper);
  expect(WIFEXITED(receiver_status) && WEXITSTATUS(receiver_status) == 0 &&
      process_exit.child == static_cast<pid_t>(wire.child) &&
      process_exit.exit_code == 0 && process_exit.error == 0,
      "a different process drives the same shell and the original process reaps it");
  destroy_terminal_local_child_reaper(process_reaper);
  ::close(channel[0]);

  std::string peer_input;
  std::string peer_output;
  cardio::primitives::conditional peer_changed;
  bool peer_eof = false;
  std::size_t resized_rows = 0;
  std::size_t resized_columns = 0;
  TerminalTransportRequest remote;
  remote.open = [&](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
        [&](std::span<unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<std::size_t> {
          while (peer_input.empty() && !peer_eof) co_await peer_changed.wait(stop);
          const auto count = std::min(bytes.size(), peer_input.size());
          std::memcpy(bytes.data(), peer_input.data(), count);
          peer_input.erase(0, count);
          co_return count;
        },
        [&](std::span<const unsigned char> bytes, cardio::cancellation) -> cardio::promise<void> {
          peer_output.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
          peer_input.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
          peer_changed.trigger();
          co_return;
        },
        [&](std::size_t rows, std::size_t columns, cardio::cancellation) -> cardio::promise<void> {
          resized_rows = rows;
          resized_columns = columns;
          co_return;
        },
        []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_connection_start_transport(test->connection, std::move(remote)),
      "start mock nonlocal channel");
  while (terminal_connection_state(test->connection).phase != TerminalConnectionPhase::connected)
    co_await test->changed.wait();
  auto remote_handoff = co_await terminal_connection_prepare_transport_handoff(test->connection);
  expect(remote_handoff.has_value() && remote_handoff->data_fd >= 0 &&
      remote_handoff->control_fd >= 0, "prepare original remote proxy sockets");
  ::close(remote_handoff->data_fd);
  ::close(remote_handoff->control_fd);
  terminal_connection_rollback_transport_handoff(test->connection);
  expect(boxrig_terminal_send_text(test->core, "REMOTE_ROLLBACK\n", 16) == BOXRIG_STATUS_OK,
      "send after remote handoff rollback");
  wake(test);
  while (contents(test).find("REMOTE_ROLLBACK") == std::string::npos) co_await test->changed.wait();
  remote_handoff = co_await terminal_connection_prepare_transport_handoff(test->connection);
  expect(remote_handoff.has_value(), "prepare remote channel again after rollback");
  Test remote_destination;
  expect(boxrig_terminal_import_checkpoint(remote_handoff->checkpoint.data(),
      remote_handoff->checkpoint.size(), 64 * 1024 * 1024, &remote_destination.core) == BOXRIG_STATUS_OK,
      "restore remote checkpoint");
  remote_destination.connection = create_terminal_connection(remote_destination.core,
      [&remote_destination] { wake(&remote_destination); },
      [&remote_destination](std::span<const unsigned char>, std::span<const unsigned char>) {
        remote_destination.changed.trigger();
      });
  expect(terminal_connection_resume_transport(remote_destination.connection,
      remote_handoff->data_fd, remote_handoff->control_fd, remote_handoff->input_finished,
      remote_handoff->encoding, remote_handoff->queue_capacity, remote_handoff->io_chunk_size),
      "adopt remote proxy sockets");
  remote_handoff->data_fd = -1;
  remote_handoff->control_fd = -1;
  auto *remote_origin = co_await terminal_connection_commit_transport_handoff(test->connection);
  expect(remote_origin != nullptr &&
      terminal_connection_state(test->connection).phase == TerminalConnectionPhase::closed,
      "source terminal releases independent origin channel");
  expect(boxrig_terminal_send_text(remote_destination.core, "REMOTE_ONE\n", 11) == BOXRIG_STATUS_OK,
      "send through remote proxy");
  wake(&remote_destination);
  while (contents(&remote_destination).find("REMOTE_ONE") == std::string::npos)
    co_await remote_destination.changed.wait();
  expect(peer_output.find("REMOTE_ONE") != std::string::npos,
      "remote origin receives proxy output");
  expect(co_await terminal_connection_resize(remote_destination.connection, 9, 90),
      "resize through control socket");
  expect(resized_rows == 9 && resized_columns == 90, "origin receives requested grid");
  auto remote_second = co_await terminal_connection_prepare_transport_handoff(remote_destination.connection);
  expect(remote_second.has_value(), "prepare second remote transfer without new origin");
  Test remote_third;
  expect(boxrig_terminal_import_checkpoint(remote_second->checkpoint.data(),
      remote_second->checkpoint.size(), 64 * 1024 * 1024, &remote_third.core) == BOXRIG_STATUS_OK,
      "restore reexported remote checkpoint");
  remote_third.connection = create_terminal_connection(remote_third.core,
      [&remote_third] { wake(&remote_third); },
      [&remote_third](std::span<const unsigned char>, std::span<const unsigned char>) {
        remote_third.changed.trigger();
      });
  expect(terminal_connection_resume_transport(remote_third.connection,
      remote_second->data_fd, remote_second->control_fd, remote_second->input_finished,
      remote_second->encoding, remote_second->queue_capacity, remote_second->io_chunk_size),
      "resume remote proxy on a third terminal");
  remote_second->data_fd = -1;
  remote_second->control_fd = -1;
  expect(co_await terminal_connection_commit_transport_handoff(remote_destination.connection) == nullptr,
      "a proxy reexport keeps the original channel instead of adding a relay");
  destroy_terminal_connection(remote_destination.connection);
  boxrig_terminal_destroy(remote_destination.core);
  expect(boxrig_terminal_send_text(remote_third.core, "REMOTE_TWO\n", 11) == BOXRIG_STATUS_OK,
      "send after remote reexport");
  wake(&remote_third);
  while (contents(&remote_third).find("REMOTE_TWO") == std::string::npos)
    co_await remote_third.changed.wait();
  peer_eof = true;
  peer_changed.trigger();
  co_await terminal_connection_wait_stopped(remote_third.connection);
  co_await terminal_transport_origin_wait(remote_origin);
  destroy_terminal_transport_origin(remote_origin);
  destroy_terminal_connection(remote_third.connection);
  boxrig_terminal_destroy(remote_third.core);
}

static cardio::promise<void> run(Test *test, cardio::dispatcher_group_glib *group, std::exception_ptr *failure) {
  try { co_await scenarios(test); }
  catch (...) { *failure = std::current_exception(); }
  terminal_connection_stop(test->connection);
  co_await terminal_connection_wait_stopped(test->connection);
  group->shutdown();
}

static cardio::promise<void> run_handoff_receiver(Test *test, cardio::dispatcher_group_glib *group,
    std::exception_ptr *failure, pid_t child) {
  try {
    while (terminal_connection_state(test->connection).phase != TerminalConnectionPhase::connected)
      co_await test->changed.wait();
    expect(boxrig_terminal_send_text(test->core, "AFTER_PROCESS_HANDOFF\n", 22) == BOXRIG_STATUS_OK,
        "send in receiving process");
    wake(test);
    const auto expected = std::to_string(child) + " AFTER_PROCESS_HANDOFF";
    while (contents(test).find(expected) == std::string::npos) co_await test->changed.wait();
    expect(boxrig_terminal_send_text(test->core, "quit\n", 5) == BOXRIG_STATUS_OK,
        "exit shell from receiving process");
    wake(test);
    co_await terminal_connection_wait_stopped(test->connection);
    expect(contents(test).find(std::to_string(child) + " quit") != std::string::npos,
        "receiving process sees final output");
  } catch (...) { *failure = std::current_exception(); }
  terminal_connection_stop(test->connection);
  co_await terminal_connection_wait_stopped(test->connection);
  group->shutdown();
}

static int handoff_receiver_main(int socket) {
  int master = -1;
  socket_master(socket, &master, false);
  HandoffWireHeader wire{};
  socket_bytes(socket, &wire, sizeof(wire), false);
  expect(wire.size > 0 && wire.size <= 64 * 1024 * 1024 && wire.child > 0,
      "bounded handoff image");
  std::vector<std::uint8_t> checkpoint(wire.size);
  socket_bytes(socket, checkpoint.data(), checkpoint.size(), false);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  Test test;
  expect(boxrig_terminal_import_checkpoint(checkpoint.data(), checkpoint.size(),
      64 * 1024 * 1024, &test.core) == BOXRIG_STATUS_OK, "restore in receiving process");
  test.connection = create_terminal_connection(test.core, [&] { wake(&test); },
      [&test](std::span<const unsigned char>, std::span<const unsigned char>) { test.changed.trigger(); });
  expect(terminal_connection_resume_local(test.connection, master, wire.input_finished != 0,
      wire.child, wire.capacity, wire.chunk), "adopt master in receiving process");
  char ready = 'R';
  socket_bytes(socket, &ready, 1, true);
  char commit = 0;
  socket_bytes(socket, &commit, 1, false);
  expect(commit == 'C', "wait for source commit");
  std::exception_ptr failure;
  auto completion = run_handoff_receiver(&test, &group, &failure, wire.child);
  dispatcher.park();
  if (test.dispatch_source) g_source_remove(test.dispatch_source);
  destroy_terminal_connection(test.connection);
  boxrig_terminal_destroy(test.core);
  ::close(socket);
  if (failure) std::rethrow_exception(failure);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--handoff-receiver") {
    try { return handoff_receiver_main(std::stoi(argv[2])); }
    catch (const std::exception &error) {
      std::fprintf(stderr, "handoff receiver: %s\n", error.what());
      return 1;
    }
  }
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  Test test;
  auto config = boxrig_terminal_config_default();
  config.output_capacity = 32;
  config.host_item_capacity = 2;
  if (boxrig_terminal_create(&config, &test.core) != BOXRIG_STATUS_OK) return 1;
  test.connection = create_terminal_connection(test.core, [&test] { wake(&test); },
      [&test](std::span<const unsigned char> raw, std::span<const unsigned char> cooked) {
        if (!raw.empty()) test.raw.append(reinterpret_cast<const char *>(raw.data()), raw.size());
        if (!cooked.empty()) test.cooked.append(reinterpret_cast<const char *>(cooked.data()), cooked.size());
        test.changed.trigger();
      });
  std::exception_ptr failure;
  auto completion = run(&test, &group, &failure);
  dispatcher.park();
  if (test.dispatch_source) g_source_remove(test.dispatch_source);
  destroy_terminal_connection(test.connection);
  boxrig_terminal_destroy(test.core);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); }
    return 1;
  }
  std::puts("Bounded PTY I/O, EOF, reconnect, startup cancellation, and reaping passed.");
  return 0;
}
