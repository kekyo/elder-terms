// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-connection.h"
#include "terminal-zmodem-auto-start.h"
#include "terminal-sessions/terminal-text-stream.h"
#include "terminal-text-send-runner.h"
#include "terminal-transfer-runner.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
// glibc 2.39's public pidfd header does not provide C++ linkage guards.
extern "C" {
#include <sys/pidfd.h>
}

namespace elder_terms {

// A linked cancellation source retains registrations on all inputs until it
// is canceled. Release them when the operation that needed the link ends.
struct CancellationScope {
  cardio::cancellation_source &source;
  ~CancellationScope() { (void)source.cancel(); }
};

struct ByteQueue {
  std::vector<unsigned char> storage;
  std::size_t head = 0;
  std::size_t length = 0;
};

struct ProxyEndpoint {
  int data_fd = -1;
  int control_fd = -1;
  cardio::primitives::mutex control_lock;
};

struct TerminalCodecEndpoint {
  int fd = -1;
  ~TerminalCodecEndpoint() { if (fd >= 0) ::close(fd); }
};

int terminal_codec_descriptor(const std::shared_ptr<TerminalCodecEndpoint> &endpoint) {
  return endpoint ? endpoint->fd : -1;
}
std::shared_ptr<TerminalCodecEndpoint> adopt_terminal_codec_descriptor(int fd) {
  if (fd < 0) return {};
  try {
    auto endpoint = std::make_shared<TerminalCodecEndpoint>();
    endpoint->fd = fd;
    return endpoint;
  } catch (...) {
    ::close(fd);
    throw;
  }
}

struct TerminalTransportOrigin {
  TerminalTransport transport;
  int data_fd = -1;
  int control_fd = -1;
  cardio::cancellation_source stop;
  std::optional<cardio::promise<void>> inbound;
  std::optional<cardio::promise<void>> outbound;
  std::optional<cardio::promise<void>> controls;
  bool finished = false;
};

static std::span<const unsigned char> front(const ByteQueue &queue) {
  return {queue.storage.data() + queue.head,
      std::min(queue.length, queue.storage.size() - queue.head)};
}

static void append(ByteQueue &queue, std::span<const unsigned char> bytes) {
  const auto tail = (queue.head + queue.length) % queue.storage.size();
  const auto first = std::min(bytes.size(), queue.storage.size() - tail);
  std::memcpy(queue.storage.data() + tail, bytes.data(), first);
  std::memcpy(queue.storage.data(), bytes.data() + first, bytes.size() - first);
  queue.length += bytes.size();
}

static void consume(ByteQueue &queue, std::size_t length) {
  queue.head = (queue.head + length) % queue.storage.size();
  queue.length -= length;
}

struct ConnectionOperation {
  boxrig_posix_pty *pty = nullptr;
  int local_fd = -1;
  std::shared_ptr<ProxyEndpoint> proxy;
  std::unique_ptr<TerminalTransportOrigin> pending_origin;
  TerminalLocalChildReaper *handoff_reaper = nullptr;
  TerminalTransport transport;
  boxrig_session *session = nullptr;
  ByteQueue incoming;
  ByteQueue outgoing;
  ByteQueue transfer_incoming;
  std::string transfer_prefetch;
  std::size_t transfer_prefetch_offset = 0;
  std::optional<unsigned char> transfer_start_control;
  std::chrono::steady_clock::time_point transfer_start_control_time;
  TerminalZmodemAutoStartDetectorState auto_start_detector;
  bool read_policy_changed = false;
  bool receive_delivery_enabled = true;
  std::size_t chunk_size = 0;
  bool eof = false;
  bool write_closed = false;
  bool peer_closed = false;
  bool finishing = false;
  std::shared_ptr<TerminalTextStream> decoder, encoder;
  TerminalCodecEndpoints codecs;
  std::vector<unsigned char> decoded;
  std::size_t decoded_offset = 0;
  bool decoder_finished = false;
  bool changing_encoding = false;
  bool service_active = false;
  bool transfer_prepared = false;
  cardio::cancellation transfer_carrier;
  bool transport_eof = false;
  cardio::primitives::conditional transfer_ready;
  bool read_paused = false;
  bool read_active = false;
  bool write_active = false;
  bool write_paused = false;
  bool handoff_preparing = false;
  bool handoff_prepared = false;
  bool handoff_committed = false;
  cardio::cancellation_source read_interrupt;
  cardio::primitives::conditional read_resumed;
  cardio::primitives::conditional stream_changed;
  std::size_t pending_operations = 0;
  cardio::cancellation_source stop;
  cardio::cancellation_source io_stop;
  cardio::cancellation_source carrier_stop;
  cardio::primitives::conditional receive_space;
  cardio::primitives::conditional send_ready;
  cardio::primitives::conditional drained;
};

struct TerminalConnection {
  boxrig_terminal *terminal = nullptr;
  std::function<void()> wake;
  std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output;
  std::function<bool(TerminalTransferDirection)> auto_start;
  TerminalConnectionState state;
  std::unique_ptr<ConnectionOperation> operation;
  std::optional<cardio::promise<void>> completion;
  std::unique_ptr<TerminalTransportOrigin> committed_origin;
};

struct TerminalLocalChildReaper {
  int process_fd = -1;
  pid_t child = 0;
  cardio::primitives::conditional ready;
  std::optional<cardio::promise<TerminalLocalChildExit>> completion;
};

static bool discard_outgoing(boxrig_terminal *terminal) {
  const uint8_t *bytes = nullptr;
  bool discarded = false;
  while (const auto count = boxrig_terminal_output_peek(terminal, &bytes)) {
    boxrig_terminal_output_consume(terminal, count);
    discarded = true;
  }
  return discarded;
}

static boxrig_transport_status receive(
    void *context, uint8_t *bytes, size_t capacity, size_t *length) {
  auto *operation = static_cast<ConnectionOperation *>(context);
  if (!operation->receive_delivery_enabled) {
    *length = 0;
    return BOXRIG_TRANSPORT_WOULD_BLOCK;
  }
  const auto available = front(operation->incoming);
  *length = std::min(capacity, available.size());
  if (*length == 0) {
    // A cancelled reader can retain the remainder of its one converted chunk.
    // Drain it after the queue, before the finalized stream reports EOF.
    if (operation->finishing && operation->decoded_offset < operation->decoded.size()) {
      *length = std::min(capacity, operation->decoded.size() - operation->decoded_offset);
      std::memcpy(bytes, operation->decoded.data() + operation->decoded_offset, *length);
      operation->decoded_offset += *length;
      return BOXRIG_TRANSPORT_OK;
    }
    return operation->eof ? BOXRIG_TRANSPORT_CLOSED : BOXRIG_TRANSPORT_WOULD_BLOCK;
  }
  std::memcpy(bytes, available.data(), *length);
  consume(operation->incoming, *length);
  operation->receive_space.trigger();
  return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status send(
    void *context, const uint8_t *bytes, size_t length, size_t *written) {
  auto *connection = static_cast<TerminalConnection *>(context);
  auto &operation = *connection->operation;
  *written = 0;
  if (operation.finishing || !connection->state.carrier_connected) {
    // Sending has ended or its carrier is down. Drain core replies without
    // forwarding them later, so retained receive data can still be parsed.
    *written = length;
    return BOXRIG_TRANSPORT_OK;
  }
  // A failed write must not close the session's receive side: a PTY can still
  // contain its final output after the child has gone away.
  if (operation.write_closed) return BOXRIG_TRANSPORT_WOULD_BLOCK;
  if (operation.write_paused) return BOXRIG_TRANSPORT_WOULD_BLOCK;
  *written = std::min(length, operation.outgoing.storage.size() - operation.outgoing.length);
  if (*written == 0) return BOXRIG_TRANSPORT_WOULD_BLOCK;
  append(operation.outgoing, {bytes, *written});
  connection->state.send_peak = std::max(connection->state.send_peak, operation.outgoing.length);
  operation.send_ready.trigger();
  return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status receive_connection(
    void *context, uint8_t *bytes, size_t capacity, size_t *length) {
  return receive(static_cast<TerminalConnection *>(context)->operation.get(), bytes, capacity, length);
}

static void fail(TerminalConnection *connection, int error) {
  if (connection->state.operation_error == 0) connection->state.operation_error = error;
  connection->state.phase = TerminalConnectionPhase::stopping;
  connection->operation->stop.cancel();
  connection->operation->io_stop.cancel();
  connection->wake();
}

static cardio::promise<void> queue_decoded(TerminalConnection *connection,
    std::vector<unsigned char> bytes, cardio::cancellation cancellation) {
  auto &operation = *connection->operation;
  operation.decoded = std::move(bytes);
  operation.decoded_offset = 0;
  while (operation.decoded_offset < operation.decoded.size()) {
    cancellation.throw_if_cancellation_requested();
    while (operation.incoming.length == operation.incoming.storage.size())
      co_await operation.receive_space.wait(cancellation);
    const auto count = std::min(operation.decoded.size() - operation.decoded_offset,
        operation.incoming.storage.size() - operation.incoming.length);
    append(operation.incoming, {operation.decoded.data() + operation.decoded_offset, count});
    operation.decoded_offset += count;
    connection->state.receive_peak = std::max(connection->state.receive_peak, operation.incoming.length);
    connection->wake();
  }
  operation.decoded.clear();
  operation.decoded_offset = 0;
}

static cardio::promise<std::vector<unsigned char>> request_conversion(
    std::shared_ptr<TerminalCodecEndpoint> endpoint, std::span<const unsigned char> bytes, bool finish);

static cardio::promise<std::vector<unsigned char>> finish_converter(
    const std::shared_ptr<TerminalCodecEndpoint> &endpoint, TerminalTextStream *converter) {
  if (endpoint) return request_conversion(endpoint, {}, true);
  return cardio::resolved(finish_terminal_text(converter).bytes);
}

static cardio::promise<std::vector<unsigned char>> finish_decoder(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  if (!operation.decoder || operation.decoder_finished) co_return std::vector<unsigned char>{};
  auto bytes = co_await finish_converter(operation.codecs.decoder, operation.decoder.get());
  operation.decoder_finished = true;
  if (connection->output && !bytes.empty()) connection->output({}, bytes);
  co_return bytes;
}

static cardio::promise<std::vector<unsigned char>> decode_received(TerminalConnection *connection,
    std::span<const unsigned char> bytes) {
  auto &operation = *connection->operation;
  std::vector<unsigned char> converted;
  if (operation.codecs.decoder) converted = co_await request_conversion(operation.codecs.decoder, bytes, false);
  else converted = convert_terminal_text(operation.decoder.get(), bytes).bytes;
  if (connection->output) connection->output(bytes, converted);
  co_return converted;
}

static cardio::promise<void> queue_pending_raw(TerminalConnection *connection, cardio::cancellation cancellation) {
  auto &operation = *connection->operation;
  if (!operation.transfer_prefetch.empty()) {
    const auto bytes = std::span(reinterpret_cast<const unsigned char *>(operation.transfer_prefetch.data()),
        operation.transfer_prefetch.size()).subspan(operation.transfer_prefetch_offset);
    auto converted = co_await decode_received(connection, bytes);
    operation.transfer_prefetch.clear();
    operation.transfer_prefetch_offset = 0;
    co_await queue_decoded(connection, std::move(converted), cancellation);
  }
  while (operation.transfer_incoming.length != 0) {
    const auto bytes = front(operation.transfer_incoming).first(
        std::min(front(operation.transfer_incoming).size(), operation.chunk_size));
    auto converted = co_await decode_received(connection, bytes);
    consume(operation.transfer_incoming, bytes.size());
    co_await queue_decoded(connection, std::move(converted), cancellation);
  }
}

static cardio::promise<std::vector<unsigned char>> finish_auto_start_prefix(TerminalConnection *connection) {
  auto &tail = connection->operation->auto_start_detector.tail;
  if (tail.empty()) co_return std::vector<unsigned char>{};
  auto converted = co_await decode_received(connection,
      {reinterpret_cast<const unsigned char *>(tail.data()), tail.size()});
  tail.clear();
  co_return converted;
}

static cardio::promise<void> read_stream(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.io_stop.get_cancellation();
  std::vector<unsigned char> buffer(operation.chunk_size);
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      operation.read_active = false;
      operation.stream_changed.trigger();
      while (operation.read_paused) co_await operation.read_resumed.wait(cancellation);
      if (operation.read_policy_changed) {
        operation.read_policy_changed = false;
        operation.read_active = true;
        co_await queue_decoded(connection, co_await finish_auto_start_prefix(connection), cancellation);
        continue;
      }
      if (!operation.transfer_prepared &&
          (operation.transfer_incoming.length != 0 || !operation.transfer_prefetch.empty())) {
        operation.read_active = true;
        co_await queue_pending_raw(connection, cancellation);
        continue;
      }
      auto *target = operation.transfer_prepared ? &operation.transfer_incoming : &operation.incoming;
      while (target->length == target->storage.size() && !operation.read_paused) {
        co_await operation.receive_space.wait(cancellation);
        target = operation.transfer_prepared ? &operation.transfer_incoming : &operation.incoming;
      }
      // A settings request can arrive while queue space is unavailable.
      if (operation.read_paused) continue;
      const auto capacity = std::min(buffer.size(), target->storage.size() - target->length);
      operation.read_interrupt = cardio::cancellations::any(cancellation);
      operation.read_active = true;
      std::size_t count = 0;
      {
        CancellationScope read_scope{operation.read_interrupt};
        try {
          count = co_await operation.transport.read(std::span(buffer).first(capacity), operation.read_interrupt.get_cancellation());
        } catch (const cardio::canceled_exception &) {
          cancellation.throw_if_cancellation_requested();
          if (operation.read_paused || operation.read_policy_changed) continue;
          throw;
        }
      }
      if (count > 0) {
        const std::span<const unsigned char> bytes(buffer.data(), static_cast<std::size_t>(count));
        if (operation.transfer_prepared) {
          append(operation.transfer_incoming, bytes);
          operation.transfer_ready.trigger();
          continue;
        }
        // A receiver can announce its X/YMODEM mode before the user presses
        // Send. Retain only the latest isolated start control; unrelated text
        // invalidates it, and the normal display/log still receives it once.
        if (!operation.service_active && bytes.size() <= 16 &&
            std::all_of(bytes.begin(), bytes.end(), [](unsigned char value) {
              return value == 0x15 || value == 'C' || value == 'G';
            })) {
          operation.transfer_start_control = bytes.back();
          operation.transfer_start_control_time = std::chrono::steady_clock::now();
        } else operation.transfer_start_control.reset();
        if (connection->auto_start && !operation.service_active && !operation.changing_encoding) {
          auto filtered = filter_terminal_zmodem_auto_start(&operation.auto_start_detector,
              {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
          if (!filtered.terminal_data.empty())
            co_await queue_decoded(connection, co_await decode_received(connection,
                {reinterpret_cast<const unsigned char *>(filtered.terminal_data.data()), filtered.terminal_data.size()}), cancellation);
          if (filtered.direction) {
            if (connection->auto_start && connection->auto_start(*filtered.direction)) {
              operation.transfer_prefetch = std::move(filtered.transfer_data);
              operation.transfer_prefetch_offset = 0;
              operation.transfer_ready.trigger();
            } else co_await queue_decoded(connection, co_await decode_received(connection,
                {reinterpret_cast<const unsigned char *>(filtered.transfer_data.data()), filtered.transfer_data.size()}), cancellation);
          }
          continue;
        }
        co_await queue_decoded(connection, co_await finish_auto_start_prefix(connection), cancellation);
        // Conversion and notifications happen once, before bounded queue and
        // parser retries. The reader retains at most one converted chunk.
        co_await queue_decoded(connection, co_await decode_received(connection, bytes), cancellation);
      } else {
        operation.transport_eof = true;
        connection->state.receive_ended = true;
        connection->wake();
        operation.read_active = false;
        operation.transfer_ready.trigger();
        operation.stream_changed.trigger();
        while (operation.transfer_prepared || operation.read_paused)
          co_await operation.read_resumed.wait(cancellation);
        co_await queue_decoded(connection, co_await finish_auto_start_prefix(connection), cancellation);
        co_await queue_pending_raw(connection, cancellation);
        co_await queue_decoded(connection, co_await finish_decoder(connection), cancellation);
        operation.eof = true;
        connection->wake();
        break;
      }
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    fail(connection, error.code().value());
  } catch (const std::length_error &) {
    fail(connection, EMSGSIZE);
  } catch (...) {
    fail(connection, ENOMEM);
  }
  operation.read_active = false;
  connection->state.receive_ended = true;
  operation.stream_changed.trigger();
  operation.transfer_ready.trigger();
  connection->wake();
}

static cardio::promise<void> write_stream(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.io_stop.get_cancellation();
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      while (operation.outgoing.length == 0 || !connection->state.carrier_connected)
        co_await operation.send_ready.wait(cancellation);
      operation.write_active = true;
      const auto bytes = front(operation.outgoing).first(std::min(front(operation.outgoing).size(), operation.chunk_size));
      auto writing = cardio::cancellations::any(cancellation, operation.carrier_stop.get_cancellation());
      CancellationScope writing_scope{writing};
      std::vector<unsigned char> encoded;
      if (operation.codecs.encoder) {
        // Release the queue before yielding. A carrier drop can discard it while the converter replies.
        std::vector<unsigned char> input(bytes.begin(), bytes.end());
        consume(operation.outgoing, bytes.size());
        encoded = co_await request_conversion(operation.codecs.encoder, input, false);
      } else {
        encoded = convert_terminal_text(operation.encoder.get(), bytes).bytes;
        consume(operation.outgoing, bytes.size());
      }
      connection->wake();
      try {
        std::size_t offset = 0;
        while (offset < encoded.size()) {
          const auto count = std::min(operation.chunk_size, encoded.size() - offset);
          co_await operation.transport.write(std::span(encoded).subspan(offset, count), writing.get_cancellation());
          offset += count;
        }
      } catch (const cardio::canceled_exception &) {
        cancellation.throw_if_cancellation_requested();
      }
      operation.write_active = false;
      operation.stream_changed.trigger();
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    if (error.code().value() == EIO || error.code().value() == EPIPE) {
      operation.write_closed = true;
      connection->wake();
    } else fail(connection, error.code().value());
  } catch (...) {
    fail(connection, ENOMEM);
  }
  operation.write_active = false;
  operation.stream_changed.trigger();
}

static cardio::promise<std::size_t> read_pty(boxrig_posix_pty *pty,
    std::span<unsigned char> bytes, const std::function<void(bool)> &activity,
    cardio::cancellation cancellation) {
  const auto fd = boxrig_posix_pty_descriptor(pty);
  for (;;) {
    co_await cardio::from_fd(fd, cardio::fd_event::read, cancellation);
    const auto count = ::read(fd, bytes.data(), bytes.size());
    if (count >= 0) {
      if (count > 0 && activity) activity(false);
      co_return static_cast<std::size_t>(count);
    }
    if (errno == EIO) co_return 0;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      throw std::system_error(errno, std::generic_category(), "read PTY");
  }
}

static cardio::promise<void> write_pty(boxrig_posix_pty *pty,
    std::span<const unsigned char> bytes, const std::function<void(bool)> &activity,
    cardio::cancellation cancellation) {
  const auto fd = boxrig_posix_pty_descriptor(pty);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    co_await cardio::from_fd(fd, cardio::fd_event::write, cancellation);
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      if (activity) activity(true);
    }
    else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      throw std::system_error(count == 0 ? EPIPE : errno, std::generic_category(), "write PTY");
  }
}

static cardio::promise<std::size_t> read_pty_fd(int fd, std::span<unsigned char> bytes,
    cardio::cancellation cancellation) {
  for (;;) {
    co_await cardio::from_fd(fd, cardio::fd_event::read, cancellation);
    const auto count = ::read(fd, bytes.data(), bytes.size());
    if (count >= 0) co_return static_cast<std::size_t>(count);
    if (errno == EIO) co_return 0;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      throw std::system_error(errno, std::generic_category(), "read transferred PTY");
  }
}

static cardio::promise<void> write_pty_fd(int fd, std::span<const unsigned char> bytes,
    cardio::cancellation cancellation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    co_await cardio::from_fd(fd, cardio::fd_event::write, cancellation);
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) offset += static_cast<std::size_t>(count);
    else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      throw std::system_error(count == 0 ? EPIPE : errno, std::generic_category(), "write transferred PTY");
  }
}

static cardio::promise<std::size_t> read_socket_fd(int fd, std::span<unsigned char> bytes,
    cardio::cancellation cancellation) {
  for (;;) {
    co_await cardio::from_fd(fd, cardio::fd_event::read, cancellation);
    const auto count = ::recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (count >= 0) co_return static_cast<std::size_t>(count);
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      throw std::system_error(errno, std::generic_category(), "read terminal proxy");
  }
}

static cardio::promise<void> write_socket_fd(int fd, std::span<const unsigned char> bytes,
    cardio::cancellation cancellation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    co_await cardio::from_fd(fd, cardio::fd_event::write, cancellation);
    const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset,
        MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) offset += static_cast<std::size_t>(count);
    else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      throw std::system_error(count == 0 ? EPIPE : errno, std::generic_category(), "write terminal proxy");
  }
}

static cardio::promise<void> read_socket_exact(int fd, std::span<unsigned char> bytes,
    cardio::cancellation cancellation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = co_await read_socket_fd(fd, bytes.subspan(offset), cancellation);
    if (count == 0) throw std::system_error(EPIPE, std::generic_category(), "terminal proxy closed");
    offset += count;
  }
}

// iconv has no exportable shift state. Keep a live converter only when a fresh
// descriptor cannot continue the stream. Separate sockets let reads and writes
// progress independently, and a second move passes the same socket onward.
static constexpr std::uint32_t finish_codec = UINT32_MAX;
static constexpr std::size_t max_codec_frame = 64U * 1024 * 1024;

static cardio::promise<void> write_codec_size(int fd, std::uint32_t size) {
  co_await write_socket_fd(fd, {reinterpret_cast<const unsigned char *>(&size), sizeof(size)}, {});
}

static cardio::promise<std::uint32_t> read_codec_size(int fd) {
  std::uint32_t size;
  co_await read_socket_exact(fd, {reinterpret_cast<unsigned char *>(&size), sizeof(size)}, {});
  co_return size;
}

static cardio::promise<std::vector<unsigned char>> read_codec_bytes(int fd, std::uint32_t size) {
  if (size > max_codec_frame) throw std::system_error(EMSGSIZE, std::generic_category());
  std::vector<unsigned char> bytes(size);
  co_await read_socket_exact(fd, bytes, {});
  co_return bytes;
}

static cardio::promise<std::vector<unsigned char>> request_conversion(
    std::shared_ptr<TerminalCodecEndpoint> endpoint, std::span<const unsigned char> bytes, bool finish) {
  if (bytes.size() > max_codec_frame) throw std::system_error(EMSGSIZE, std::generic_category());
  co_await write_codec_size(endpoint->fd, finish ? finish_codec : static_cast<std::uint32_t>(bytes.size()));
  co_await write_socket_fd(endpoint->fd, bytes, {});
  const auto size = co_await read_codec_size(endpoint->fd);
  co_return co_await read_codec_bytes(endpoint->fd, size);
}

static cardio::promise<void> serve_converter(std::shared_ptr<TerminalCodecEndpoint> socket,
    std::shared_ptr<TerminalTextStream> converter) {
  try {
    for (;;) {
      const auto size = co_await read_codec_size(socket->fd);
      TerminalTextConversionResult result;
      if (size == finish_codec) {
        result = finish_terminal_text(converter.get());
      } else {
        const auto input = co_await read_codec_bytes(socket->fd, size);
        result = convert_terminal_text(converter.get(), input);
      }
      if (result.bytes.size() > max_codec_frame) break;
      co_await write_codec_size(socket->fd, static_cast<std::uint32_t>(result.bytes.size()));
      co_await write_socket_fd(socket->fd, result.bytes, {});
      if (size == finish_codec) break;
    }
  } catch (const std::exception &) {
    // Closing the last receiver or a conversion failure ends this stream.
  }
  ::shutdown(socket->fd, SHUT_RDWR);
}

static TerminalCodecHandoff prepare_codecs(const ConnectionOperation &operation, const std::string &encoding) {
  constexpr std::string_view stateless[] = {"UTF-8", "ASCII", "ISO-8859-1", "ISO-8859-15",
      "WINDOWS-1252", "WINDOWS-1251", "KOI8-R", "CP437", "SHIFT-JIS", "CP932", "EUC-JP", "GB18030",
      "BIG5", "EUC-KR"};
  const bool restartable = std::ranges::find(stateless, encoding) != std::end(stateless);
  TerminalCodecHandoff handoff;
  const auto retain = [&](const std::shared_ptr<TerminalTextStream> &converter,
      const std::shared_ptr<TerminalCodecEndpoint> &existing) -> std::shared_ptr<TerminalCodecEndpoint> {
    if (existing || (restartable && !terminal_text_stream_has_pending(converter.get()))) return existing;
    auto client = std::make_shared<TerminalCodecEndpoint>();
    auto server = std::make_shared<TerminalCodecEndpoint>();
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0)
      throw std::system_error(errno, std::generic_category(), "terminal character converter");
    client->fd = fds[0];
    server->fd = fds[1];
    handoff.origins.push_back(std::make_shared<cardio::promise<void>>(serve_converter(std::move(server), converter)));
    return client;
  };
  handoff.endpoints.decoder = retain(operation.decoder, operation.codecs.decoder);
  handoff.endpoints.encoder = retain(operation.encoder, operation.codecs.encoder);
  return handoff;
}

static cardio::promise<void> proxy_control(ProxyEndpoint *endpoint, std::uint32_t operation,
    std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
  auto acquiring = endpoint->control_lock.lock(cancellation);
  auto lock = std::move(co_await acquiring);
  const std::array<std::uint32_t, 3> request{
      operation, static_cast<std::uint32_t>(rows), static_cast<std::uint32_t>(columns)};
  co_await write_socket_fd(endpoint->control_fd,
      {reinterpret_cast<const unsigned char *>(request.data()), sizeof(request)}, cancellation);
  std::int32_t result = 0;
  co_await read_socket_exact(endpoint->control_fd,
      {reinterpret_cast<unsigned char *>(&result), sizeof(result)}, cancellation);
  if (result != 0) throw std::system_error(result, std::generic_category(), "terminal proxy control");
}

static TerminalTransport proxy_transport(std::shared_ptr<ProxyEndpoint> endpoint) {
  TerminalTransport transport;
  transport.read = [endpoint](std::span<unsigned char> bytes, cardio::cancellation stop) {
    return read_socket_fd(endpoint->data_fd, bytes, stop);
  };
  transport.write = [endpoint](std::span<const unsigned char> bytes, cardio::cancellation stop) {
    return write_socket_fd(endpoint->data_fd, bytes, stop);
  };
  transport.resize = [endpoint](std::size_t rows, std::size_t columns, cardio::cancellation stop) {
    return proxy_control(endpoint.get(), 1, rows, columns, stop);
  };
  transport.send_break = [endpoint](cardio::cancellation stop) {
    return proxy_control(endpoint.get(), 2, 0, 0, stop);
  };
  transport.close = [endpoint]() -> cardio::promise<void> {
    if (endpoint->data_fd >= 0) ::close(std::exchange(endpoint->data_fd, -1));
    if (endpoint->control_fd >= 0) ::close(std::exchange(endpoint->control_fd, -1));
    co_return;
  };
  return transport;
}

static cardio::promise<void> pump_proxy_output(TerminalTransportOrigin *origin) {
  const auto stop = origin->stop.get_cancellation();
  try {
    std::array<unsigned char, 4096> bytes{};
    for (;;) {
      const auto count = co_await origin->transport.read(bytes, stop);
      if (count == 0) break;
      co_await write_socket_fd(origin->data_fd, std::span(bytes).first(count), stop);
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &) {
  }
  ::shutdown(origin->data_fd, SHUT_WR);
}

static cardio::promise<void> pump_proxy_input(TerminalTransportOrigin *origin) {
  const auto stop = origin->stop.get_cancellation();
  try {
    std::array<unsigned char, 4096> bytes{};
    for (;;) {
      const auto count = co_await read_socket_fd(origin->data_fd, bytes, stop);
      if (count == 0) break;
      co_await origin->transport.write(std::span(bytes).first(count), stop);
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &) {
  }
  origin->stop.cancel();
}

static cardio::promise<void> pump_proxy_controls(TerminalTransportOrigin *origin) {
  const auto stop = origin->stop.get_cancellation();
  try {
    for (;;) {
      std::array<std::uint32_t, 3> request{};
      co_await read_socket_exact(origin->control_fd,
          {reinterpret_cast<unsigned char *>(request.data()), sizeof(request)}, stop);
      std::int32_t result = 0;
      try {
        if (request[0] == 1) co_await origin->transport.resize(request[1], request[2], stop);
        else if (request[0] == 2 && origin->transport.send_break)
          co_await origin->transport.send_break(stop);
        else result = ENOTSUP;
      } catch (const std::system_error &error) {
        result = error.code().value();
      } catch (const cardio::canceled_exception &) {
        throw;
      } catch (...) {
        result = EIO;
      }
      co_await write_socket_fd(origin->control_fd,
          {reinterpret_cast<const unsigned char *>(&result), sizeof(result)}, stop);
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &) {
  }
}

static void create_session(TerminalConnection *connection, std::optional<bool> resumed_eof = std::nullopt) {
  auto &operation = *connection->operation;
  // All grid changes await the transport explicitly. Registering a synchronous
  // resize callback here would send the same request twice on remote channels.
  const boxrig_session_transport transport{connection, receive_connection, send, nullptr, nullptr};
  auto config = boxrig_session_config_default();
  config.input_capacity = operation.incoming.storage.size();
  config.maximum_read_bytes_per_dispatch = operation.incoming.storage.size();
  const auto status = resumed_eof.has_value()
      ? boxrig_session_create_resumed(connection->terminal, &transport, &config, *resumed_eof, &operation.session)
      : boxrig_session_create(connection->terminal, &transport, &config, &operation.session);
  if (status != BOXRIG_SESSION_OK) throw std::bad_alloc();
}

struct SpawnResult {
  boxrig_posix_pty *pty = nullptr;
  boxrig_posix_pty_spawn_result diagnostic{};
};

static void terminate_child(pid_t child) {
  // The sole reaper has not run, so neither the PID nor its group can be
  // recycled. The direct signal also covers cancellation before setsid().
  (void)::kill(-child, SIGKILL);
  (void)::kill(child, SIGKILL);
}

static cardio::promise<TerminalLocalChildExit> reap_transferred_child(TerminalLocalChildReaper *reaper) {
  TerminalLocalChildExit exit;
  exit.child = reaper->child;
  try {
    co_await cardio::from_fd(reaper->process_fd, cardio::fd_event::read, {});
    int status = 0;
    pid_t result = -1;
    do result = ::waitpid(reaper->child, &status, WNOHANG);
    while (result < 0 && errno == EINTR);
    if (result < 0) exit.error = errno;
    else if (result == 0) exit.error = EAGAIN;
    else if (WIFEXITED(status)) exit.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit.signal_number = WTERMSIG(status);
  } catch (const std::system_error &error) {
    exit.error = error.code().value();
  } catch (...) {
    exit.error = EIO;
  }
  co_return exit;
}

static bool finish_input(TerminalConnection *connection) {
  discard_outgoing(connection->terminal);
  const auto status = boxrig_terminal_finish_input(connection->terminal);
  if (status == BOXRIG_STATUS_WOULD_BLOCK) return false;
  if (status != BOXRIG_STATUS_OK) connection->state.operation_error = EIO;
  connection->operation->finishing = false;
  connection->operation->drained.trigger();
  return true;
}

static cardio::promise<void> finish_stream(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  try {
    const auto prefix = co_await finish_auto_start_prefix(connection);
    operation.decoded.insert(operation.decoded.end(), prefix.begin(), prefix.end());
    // A successful binary transfer may have prefetched ordinary text. Keep
    // that accepted suffix even if shutdown cancelled the resumed reader.
    if (!operation.transfer_prepared) {
      if (!operation.transfer_prefetch.empty()) {
        const auto bytes = std::span(reinterpret_cast<const unsigned char *>(operation.transfer_prefetch.data()),
            operation.transfer_prefetch.size()).subspan(operation.transfer_prefetch_offset);
        const auto converted = co_await decode_received(connection, bytes);
        operation.transfer_prefetch.clear();
        operation.transfer_prefetch_offset = 0;
        operation.decoded.insert(operation.decoded.end(), converted.begin(), converted.end());
      }
      while (operation.transfer_incoming.length != 0) {
        const auto bytes = front(operation.transfer_incoming);
        const auto converted = co_await decode_received(connection, bytes);
        consume(operation.transfer_incoming, bytes.size());
        operation.decoded.insert(operation.decoded.end(), converted.begin(), converted.end());
      }
    }
    if (operation.transport.finish_read) {
      const auto raw = operation.transport.finish_read();
      if (!raw.empty()) {
        const auto converted = co_await decode_received(connection, raw);
        operation.decoded.insert(operation.decoded.end(), converted.begin(), converted.end());
      }
    }
    auto tail = co_await finish_decoder(connection);
    operation.decoded.insert(operation.decoded.end(), tail.begin(), tail.end());
  } catch (const std::system_error &error) {
    connection->state.operation_error = error.code().value();
  }
  // Cancellation also ends a stream. Its incomplete UTF-8/control syntax
  // cannot become the prefix of the next connection's input.
  operation.finishing = true;
  operation.eof = true;
  if (operation.session) {
    connection->wake();
    while (!operation.peer_closed) co_await operation.drained.wait();
  }
  while (!finish_input(connection)) {
    connection->wake();
    co_await operation.drained.wait();
  }
  boxrig_session_destroy(operation.session);
  operation.session = nullptr;
}

static cardio::promise<void> run_local(TerminalConnection *connection, TerminalLocalCommand command) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.stop.get_cancellation();
  int process = -1;
  std::optional<cardio::promise<void>> reader;
  std::optional<cardio::promise<void>> writer;
  try {
    operation.decoder = create_terminal_text_decoder(command.encoding);
    operation.encoder = create_terminal_text_encoder(command.encoding);
    connection->state.encoding = command.encoding;
    size_t rows = 0, columns = 0;
    boxrig_terminal_size(connection->terminal, &rows, &columns);
    auto activity = std::move(command.activity);
    const auto spawned = co_await cardio::promises::start_new([command = std::move(command), rows, columns]() mutable {
      std::vector<char *> arguments;
      for (auto &argument : command.arguments) arguments.push_back(argument.data());
      arguments.push_back(nullptr);
      SpawnResult result;
      boxrig_posix_pty_spawn_with_diagnostic(arguments.data(), command.terminal_type.c_str(), rows, columns,
          &result.pty, &result.diagnostic);
      return result;
    });
    operation.pty = spawned.pty;
    connection->state.spawn = spawned.diagnostic;
    if (operation.pty) {
      connection->state.child = boxrig_posix_pty_child(operation.pty);
      process = pidfd_open(connection->state.child, 0);
      if (process < 0) throw std::system_error(errno, std::generic_category(), "observe PTY child");
      cancellation.throw_if_cancellation_requested();
      for (;;) {
        if (boxrig_posix_pty_get_spawn_result(operation.pty, &connection->state.spawn) != 0)
          throw std::system_error(errno, std::generic_category(), "PTY spawn diagnostic");
        if (connection->state.spawn.state != BOXRIG_POSIX_PTY_SPAWN_PENDING) break;
        co_await cardio::from_fd(boxrig_posix_pty_spawn_descriptor(operation.pty), cardio::fd_event::read, cancellation);
      }
      // GTK may have completed a layout while synchronous preparation ran.
      // Start this generation at that confirmed grid before exposing input.
      boxrig_terminal_size(connection->terminal, &rows, &columns);
      if (boxrig_posix_pty_resize(operation.pty, rows, columns) != 0)
        throw std::system_error(errno, std::generic_category(), "initial PTY grid");
      operation.transport.read = [pty = operation.pty, activity](std::span<unsigned char> bytes, cardio::cancellation stop) {
        return read_pty(pty, bytes, activity, stop);
      };
      operation.transport.write = [pty = operation.pty, activity](std::span<const unsigned char> bytes, cardio::cancellation stop) {
        return write_pty(pty, bytes, activity, stop);
      };
      operation.transport.resize = [pty = operation.pty](std::size_t height, std::size_t width,
          cardio::cancellation stop) -> cardio::promise<void> {
        stop.throw_if_cancellation_requested();
        if (boxrig_posix_pty_resize(pty, height, width) != 0)
          throw std::system_error(errno, std::generic_category(), "resize PTY");
        co_return;
      };
      create_session(connection);
      if (connection->state.spawn.state == BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED) {
        connection->state.phase = TerminalConnectionPhase::connected;
        connection->wake();
      }
      reader.emplace(read_stream(connection));
      writer.emplace(write_stream(connection));
      co_await cardio::from_fd(process, cardio::fd_event::read, cancellation);
      // The child has exited. Keep reading until EOF and let the session drain
      // its parser tail before reporting disconnection.
      co_await *reader;
      while (!operation.peer_closed) co_await operation.drained.wait(cancellation);
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    connection->state.operation_error = error.code().value();
    connection->state.operation_error_message = error.what();
  } catch (...) {
    connection->state.operation_error = ENOMEM;
  }
  operation.io_stop.cancel();
  if (operation.pty) {
    if (!operation.handoff_committed &&
        (connection->state.cancelled || connection->state.operation_error != 0))
      terminate_child(connection->state.child);
    if (reader) co_await *reader;
    if (writer) co_await *writer;
    while (operation.pending_operations != 0) co_await operation.drained.wait();
    if (!operation.handoff_committed) {
      // One owner, one wait path, including failure to allocate a pidfd. Even
      // that exceptional blocking wait cannot hold the GTK dispatcher.
      const auto reaped = co_await cardio::promises::start_new([pty = operation.pty] {
        int status = 0;
        const int error = boxrig_posix_pty_wait(pty, &status) == 0 ? 0 : errno;
        return std::pair{status, error};
      });
      connection->state.reaped = reaped.second == 0;
      if (reaped.second != 0) connection->state.operation_error = reaped.second;
      else if (WIFEXITED(reaped.first)) connection->state.exit_code = WEXITSTATUS(reaped.first);
      else if (WIFSIGNALED(reaped.first)) connection->state.signal_number = WTERMSIG(reaped.first);
    }
    boxrig_posix_pty_get_spawn_result(operation.pty, &connection->state.spawn);
  }
  if (operation.handoff_committed && operation.handoff_reaper) {
    operation.handoff_reaper->process_fd = process;
    process = -1;
    operation.handoff_reaper->completion.emplace(reap_transferred_child(operation.handoff_reaper));
    operation.handoff_reaper->ready.trigger();
  }
  if (process >= 0) ::close(process);
  if (operation.handoff_committed) {
    boxrig_session_destroy(operation.session);
    operation.session = nullptr;
  } else co_await finish_stream(connection);
  boxrig_posix_pty_destroy(operation.pty);
  operation.pty = nullptr;
  operation.transport = {};
  connection->state.phase = TerminalConnectionPhase::closed;
  connection->wake();
}

static cardio::promise<void> run_resumed_local(TerminalConnection *connection, int master_fd,
    bool input_finished) {
  auto &operation = *connection->operation;
  operation.local_fd = master_fd;
  const auto cancellation = operation.stop.get_cancellation();
  std::optional<cardio::promise<void>> reader;
  std::optional<cardio::promise<void>> writer;
  try {
    operation.decoder = create_terminal_text_decoder(connection->state.encoding);
    operation.encoder = create_terminal_text_encoder(connection->state.encoding);
    operation.transport.read = [master_fd](std::span<unsigned char> bytes, cardio::cancellation stop) {
      return read_pty_fd(master_fd, bytes, stop);
    };
    operation.transport.write = [master_fd](std::span<const unsigned char> bytes, cardio::cancellation stop) {
      return write_pty_fd(master_fd, bytes, stop);
    };
    operation.transport.resize = [master_fd](std::size_t rows, std::size_t columns,
        cardio::cancellation stop) -> cardio::promise<void> {
      stop.throw_if_cancellation_requested();
      struct winsize size{};
      size.ws_row = static_cast<unsigned short>(rows);
      size.ws_col = static_cast<unsigned short>(columns);
      if (::ioctl(master_fd, TIOCSWINSZ, &size) != 0)
        throw std::system_error(errno, std::generic_category(), "resize transferred PTY");
      co_return;
    };
    create_session(connection, input_finished);
    connection->state.phase = TerminalConnectionPhase::connected;
    connection->wake();
    reader.emplace(read_stream(connection));
    writer.emplace(write_stream(connection));
    co_await *reader;
    while (!operation.peer_closed) co_await operation.drained.wait(cancellation);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    connection->state.operation_error = error.code().value();
    connection->state.operation_error_message = error.what();
  } catch (...) {
    connection->state.operation_error = ENOMEM;
  }
  operation.io_stop.cancel();
  if (!operation.handoff_committed &&
      (connection->state.cancelled || connection->state.operation_error != 0))
    terminate_child(connection->state.child);
  if (reader) co_await *reader;
  if (writer) co_await *writer;
  while (operation.pending_operations != 0) co_await operation.drained.wait();
  if (operation.handoff_committed) {
    boxrig_session_destroy(operation.session);
    operation.session = nullptr;
  } else co_await finish_stream(connection);
  ::close(master_fd);
  operation.local_fd = -1;
  operation.transport = {};
  connection->state.phase = TerminalConnectionPhase::closed;
  connection->wake();
}

static cardio::promise<void> watch_carrier(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.io_stop.get_cancellation();
  try {
    co_await operation.transport.watch_carrier([connection, cancellation](bool connected) {
      if (cancellation.is_cancellation_requested() || connection->state.carrier_connected == connected) return;
      auto &current = *connection->operation;
      connection->state.carrier_connected = connected;
      if (!connected) {
        current.transfer_start_control.reset();
        ++connection->state.carrier_losses;
        current.carrier_stop.cancel();
        consume(current.outgoing, current.outgoing.length);
        discard_outgoing(connection->terminal);
        // All writers retain converted bytes, never a borrowed encoder. Drop
        // its old partial UTF-8 and shift state before accepting a new carrier.
        current.codecs.encoder.reset();
        current.encoder = create_terminal_text_encoder(connection->state.encoding);
      } else current.carrier_stop = {};
      current.send_ready.trigger();
      connection->wake();
    }, cancellation);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    fail(connection, error.code().value());
  } catch (...) {
    fail(connection, EIO);
  }
}

static cardio::promise<void> run_transport(TerminalConnection *connection, TerminalTransportRequest request,
    std::optional<bool> resumed_eof) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.stop.get_cancellation();
  std::optional<cardio::promise<void>> reader;
  std::optional<cardio::promise<void>> writer;
  std::optional<cardio::promise<void>> carrier;
  try {
    operation.decoder = create_terminal_text_decoder(request.encoding);
    operation.encoder = create_terminal_text_encoder(request.encoding);
    connection->state.encoding = request.encoding;
    size_t rows = 0, columns = 0;
    boxrig_terminal_size(connection->terminal, &rows, &columns);
    if (!resumed_eof.has_value()) operation.transport = co_await request.open(rows, columns, cancellation);
    if (!operation.transport.read || !operation.transport.write || !operation.transport.resize || !operation.transport.close)
      throw std::system_error(EINVAL, std::generic_category(), "terminal transport operations");
    size_t current_rows = 0, current_columns = 0;
    boxrig_terminal_size(connection->terminal, &current_rows, &current_columns);
    if (current_rows != rows || current_columns != columns)
      co_await operation.transport.resize(current_rows, current_columns, cancellation);
    cancellation.throw_if_cancellation_requested();
    create_session(connection, resumed_eof);
    connection->state.phase = TerminalConnectionPhase::connected;
    connection->wake();
    reader.emplace(read_stream(connection));
    writer.emplace(write_stream(connection));
    if (operation.transport.watch_carrier) carrier.emplace(watch_carrier(connection));
    co_await *reader;
    while (!operation.peer_closed) co_await operation.drained.wait(cancellation);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    connection->state.operation_error = error.code().value();
    connection->state.operation_error_message = error.what();
  } catch (const std::exception &error) {
    connection->state.operation_error = EIO;
    connection->state.operation_error_message = error.what();
  } catch (...) {
    connection->state.operation_error = EIO;
  }
  operation.io_stop.cancel();
  if (reader) co_await *reader;
  if (writer) co_await *writer;
  if (carrier) co_await *carrier;
  while (operation.pending_operations != 0) co_await operation.drained.wait();
  if (operation.handoff_committed) {
    boxrig_session_destroy(operation.session);
    operation.session = nullptr;
    if (operation.pending_origin) {
      // The channel stays live. Its next read continues any partial protocol frame.
      operation.pending_origin->transport = std::move(operation.transport);
      auto *origin = operation.pending_origin.get();
      origin->outbound.emplace(pump_proxy_output(origin));
      origin->inbound.emplace(pump_proxy_input(origin));
      origin->controls.emplace(pump_proxy_controls(origin));
      connection->committed_origin = std::move(operation.pending_origin);
    } else if (operation.transport.close) co_await operation.transport.close();
  } else {
    co_await finish_stream(connection);
    if (operation.transport.close) co_await operation.transport.close();
  }
  operation.transport = {};
  connection->state.phase = TerminalConnectionPhase::closed;
  connection->wake();
}

TerminalConnection *create_terminal_connection(
    boxrig_terminal *terminal, std::function<void()> wake,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output) {
  auto connection = std::make_unique<TerminalConnection>();
  connection->terminal = terminal;
  connection->wake = std::move(wake);
  connection->output = std::move(output);
  return connection.release();
}

void destroy_terminal_connection(TerminalConnection *connection) {
  delete connection;
}

static bool prepare_connection(TerminalConnection *connection, std::size_t queue_capacity, std::size_t io_chunk_size) {
  if ((connection->state.phase != TerminalConnectionPhase::idle &&
       connection->state.phase != TerminalConnectionPhase::closed) ||
      connection->state.generation == std::numeric_limits<std::uint64_t>::max() ||
      queue_capacity == 0 || io_chunk_size == 0 || queue_capacity > 1024 * 1024) return false;
  const auto generation = connection->state.generation + 1;
  connection->completion.reset();
  connection->operation = std::make_unique<ConnectionOperation>();
  connection->operation->incoming.storage.resize(queue_capacity);
  connection->operation->outgoing.storage.resize(queue_capacity);
  connection->operation->transfer_incoming.storage.resize(queue_capacity);
  connection->operation->chunk_size = std::min(io_chunk_size, queue_capacity);
  connection->state = {};
  connection->state.generation = generation;
  connection->state.phase = TerminalConnectionPhase::starting;
  discard_outgoing(connection->terminal);
  return true;
}

bool terminal_connection_start_local(TerminalConnection *connection, TerminalLocalCommand command) {
  if (!prepare_connection(connection, command.queue_capacity, command.io_chunk_size)) return false;
  connection->completion.emplace(run_local(connection, std::move(command)));
  connection->wake();
  return true;
}

bool terminal_connection_resume_local(TerminalConnection *connection, int master_fd,
    bool input_finished, pid_t child, std::string encoding, std::size_t queue_capacity, std::size_t io_chunk_size,
    TerminalCodecEndpoints codecs) {
  if (master_fd < 0 || child <= 0 || encoding.empty() ||
      !prepare_connection(connection, queue_capacity, io_chunk_size)) return false;
  connection->operation->codecs = std::move(codecs);
  connection->state.child = child;
  connection->state.encoding = std::move(encoding);
  connection->state.spawn.state = BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED;
  connection->completion.emplace(run_resumed_local(connection, master_fd, input_finished));
  connection->wake();
  return true;
}

bool terminal_connection_resume_transport(TerminalConnection *connection, int data_fd,
    int control_fd, bool input_finished, std::string encoding,
    std::size_t queue_capacity, std::size_t io_chunk_size, TerminalCodecEndpoints codecs) {
  if (data_fd < 0 || control_fd < 0 || encoding.empty() ||
      !prepare_connection(connection, queue_capacity, io_chunk_size)) return false;
  auto endpoint = std::make_shared<ProxyEndpoint>();
  endpoint->data_fd = data_fd;
  endpoint->control_fd = control_fd;
  connection->operation->codecs = std::move(codecs);
  connection->operation->proxy = endpoint;
  connection->operation->transport = proxy_transport(endpoint);
  TerminalTransportRequest request;
  request.encoding = std::move(encoding);
  connection->completion.emplace(run_transport(connection, std::move(request), input_finished));
  connection->wake();
  return true;
}

bool terminal_connection_resume_serial(TerminalConnection *connection, int fd,
    bool input_finished, const TerminalTransportRequest &request, TerminalCodecEndpoints codecs) {
  if (fd < 0 || !request.adopt_serial_port || request.encoding.empty() ||
      !prepare_connection(connection, request.queue_capacity, request.io_chunk_size)) return false;
  connection->operation->codecs = std::move(codecs);
  connection->operation->transport = request.adopt_serial_port(fd);
  connection->completion.emplace(run_transport(connection, request, input_finished));
  connection->wake();
  return true;
}

bool terminal_connection_start_transport(TerminalConnection *connection, TerminalTransportRequest request) {
  if (!request.open || !prepare_connection(connection, request.queue_capacity, request.io_chunk_size)) return false;
  connection->completion.emplace(run_transport(connection, std::move(request), std::nullopt));
  connection->wake();
  return true;
}

void terminal_connection_stop(TerminalConnection *connection) {
  if (connection->state.phase == TerminalConnectionPhase::idle ||
      connection->state.phase == TerminalConnectionPhase::closed) return;
  if (connection->operation->pending_origin)
    terminal_connection_rollback_transport_handoff(connection);
  connection->state.cancelled = true;
  connection->operation->receive_delivery_enabled = true;
  connection->state.phase = TerminalConnectionPhase::stopping;
  connection->operation->stop.cancel();
  connection->operation->io_stop.cancel();
  connection->wake();
}

cardio::promise<void> terminal_connection_wait_stopped(TerminalConnection *connection) {
  if (connection->completion) co_await *connection->completion;
}

void terminal_connection_rollback_local_handoff(TerminalConnection *connection) {
  if (!connection || !connection->operation || connection->operation->handoff_committed) return;
  auto &operation = *connection->operation;
  operation.handoff_preparing = false;
  operation.handoff_prepared = false;
  operation.read_paused = false;
  operation.read_resumed.trigger();
  connection->wake();
}

void terminal_connection_rollback_transport_handoff(TerminalConnection *connection) {
  if (!connection || !connection->operation || connection->operation->handoff_committed) return;
  auto &operation = *connection->operation;
  if (operation.pending_origin) {
    if (operation.pending_origin->data_fd >= 0) ::close(operation.pending_origin->data_fd);
    if (operation.pending_origin->control_fd >= 0) ::close(operation.pending_origin->control_fd);
    operation.pending_origin.reset();
  }
  terminal_connection_rollback_local_handoff(connection);
}

cardio::promise<std::optional<TerminalLocalHandoff>> terminal_connection_prepare_local_handoff(
    TerminalConnection *connection) {
  if (!connection || !connection->operation ||
      connection->state.phase != TerminalConnectionPhase::connected ||
      (!connection->operation->pty && connection->operation->local_fd < 0) ||
      !connection->operation->session ||
      connection->operation->handoff_preparing || connection->operation->handoff_prepared ||
      connection->operation->pending_operations != 0 ||
      connection->operation->transfer_prepared || connection->operation->service_active ||
      connection->operation->changing_encoding) co_return std::nullopt;
  auto &operation = *connection->operation;
  operation.handoff_preparing = true;
  operation.read_paused = true;
  operation.read_interrupt.cancel();
  operation.receive_space.trigger();
  const auto cancellation = operation.io_stop.get_cancellation();
  try {
    while (operation.read_active) co_await operation.stream_changed.wait(cancellation);
    connection->wake();
    while (operation.incoming.length != 0 || operation.outgoing.length != 0 ||
        operation.write_active || operation.decoded_offset < operation.decoded.size() ||
        (boxrig_session_interests(operation.session) & BOXRIG_SESSION_INTEREST_WRITE) != 0)
      co_await operation.stream_changed.wait(cancellation);
    cancellation.throw_if_cancellation_requested();
    if (operation.pending_operations != 0 || operation.eof || operation.transport_eof ||
        operation.transfer_incoming.length != 0 || !operation.transfer_prefetch.empty() ||
        !operation.auto_start_detector.tail.empty()) {
      terminal_connection_rollback_local_handoff(connection);
      co_return std::nullopt;
    }
    std::uint8_t *data = nullptr;
    std::size_t length = 0;
    bool input_finished = false;
    const auto status = boxrig_session_export_checkpoint(operation.session, 64 * 1024 * 1024,
        &data, &length, &input_finished);
    if (status != BOXRIG_STATUS_OK) {
      terminal_connection_rollback_local_handoff(connection);
      co_return std::nullopt;
    }
    TerminalLocalHandoff handoff;
    try {
      handoff.checkpoint.assign(data, data + length);
    } catch (...) {
      boxrig_checkpoint_free(data);
      throw;
    }
    boxrig_checkpoint_free(data);
    handoff.codecs = prepare_codecs(operation, connection->state.encoding);
    const int source_fd = operation.pty ? boxrig_posix_pty_descriptor(operation.pty) : operation.local_fd;
    const int fd = ::fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
      terminal_connection_rollback_local_handoff(connection);
      co_return std::nullopt;
    }
    handoff.input_finished = input_finished;
    handoff.master_fd = fd;
    handoff.child = connection->state.child;
    handoff.encoding = connection->state.encoding;
    handoff.queue_capacity = operation.incoming.storage.size();
    handoff.io_chunk_size = operation.chunk_size;
    operation.handoff_preparing = false;
    operation.handoff_prepared = true;
    co_return handoff;
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
  }
  terminal_connection_rollback_local_handoff(connection);
  co_return std::nullopt;
}

cardio::promise<std::optional<TerminalTransportHandoff>> terminal_connection_prepare_transport_handoff(
    TerminalConnection *connection) {
  if (!connection || !connection->operation ||
      connection->state.phase != TerminalConnectionPhase::connected ||
      connection->operation->pty || connection->operation->local_fd >= 0 ||
      !connection->operation->transport.close || !connection->operation->session ||
      connection->operation->handoff_preparing || connection->operation->handoff_prepared ||
      connection->operation->pending_operations != 0 ||
      connection->operation->transfer_prepared || connection->operation->service_active ||
      connection->operation->changing_encoding) co_return std::nullopt;
  auto &operation = *connection->operation;
  operation.handoff_preparing = true;
  operation.read_paused = true;
  operation.read_interrupt.cancel();
  operation.receive_space.trigger();
  const auto cancellation = operation.io_stop.get_cancellation();
  try {
    while (operation.read_active) co_await operation.stream_changed.wait(cancellation);
    connection->wake();
    while (operation.incoming.length != 0 || operation.outgoing.length != 0 ||
        operation.write_active || operation.decoded_offset < operation.decoded.size() ||
        (boxrig_session_interests(operation.session) & BOXRIG_SESSION_INTEREST_WRITE) != 0)
      co_await operation.stream_changed.wait(cancellation);
    cancellation.throw_if_cancellation_requested();
    if (operation.pending_operations != 0 || operation.eof || operation.transport_eof ||
        operation.transfer_incoming.length != 0 || !operation.transfer_prefetch.empty() ||
        !operation.auto_start_detector.tail.empty()) {
      terminal_connection_rollback_transport_handoff(connection);
      co_return std::nullopt;
    }
    std::uint8_t *data = nullptr;
    std::size_t length = 0;
    bool input_finished = false;
    const auto status = boxrig_session_export_checkpoint(operation.session, 64 * 1024 * 1024,
        &data, &length, &input_finished);
    if (status != BOXRIG_STATUS_OK) {
      terminal_connection_rollback_transport_handoff(connection);
      co_return std::nullopt;
    }
    TerminalTransportHandoff handoff;
    try { handoff.checkpoint.assign(data, data + length); }
    catch (...) { boxrig_checkpoint_free(data); throw; }
    boxrig_checkpoint_free(data);
    handoff.input_finished = input_finished;
    handoff.encoding = connection->state.encoding;
    handoff.queue_capacity = operation.incoming.storage.size();
    handoff.io_chunk_size = operation.chunk_size;
    handoff.codecs = prepare_codecs(operation, connection->state.encoding);
    if (operation.transport.duplicate_serial_port) {
      handoff.data_fd = operation.transport.duplicate_serial_port();
    } else if (operation.proxy) {
      handoff.data_fd = ::fcntl(operation.proxy->data_fd, F_DUPFD_CLOEXEC, 0);
      if (handoff.data_fd >= 0)
        handoff.control_fd = ::fcntl(operation.proxy->control_fd, F_DUPFD_CLOEXEC, 0);
    } else {
      auto pending_origin = std::make_unique<TerminalTransportOrigin>();
      int data_pair[2]{-1, -1};
      int control_pair[2]{-1, -1};
      if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, data_pair) == 0 &&
          ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, control_pair) == 0) {
        pending_origin->data_fd = data_pair[0];
        pending_origin->control_fd = control_pair[0];
        operation.pending_origin = std::move(pending_origin);
        handoff.data_fd = data_pair[1];
        handoff.control_fd = control_pair[1];
      } else {
        if (data_pair[0] >= 0) ::close(data_pair[0]);
        if (data_pair[1] >= 0) ::close(data_pair[1]);
        if (control_pair[0] >= 0) ::close(control_pair[0]);
        if (control_pair[1] >= 0) ::close(control_pair[1]);
      }
    }
    if (handoff.data_fd < 0 || (!operation.transport.duplicate_serial_port && handoff.control_fd < 0)) {
      if (handoff.data_fd >= 0) ::close(handoff.data_fd);
      if (handoff.control_fd >= 0) ::close(handoff.control_fd);
      terminal_connection_rollback_transport_handoff(connection);
      co_return std::nullopt;
    }
    operation.handoff_preparing = false;
    operation.handoff_prepared = true;
    co_return handoff;
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
  }
  terminal_connection_rollback_transport_handoff(connection);
  co_return std::nullopt;
}

cardio::promise<TerminalTransportOrigin *> terminal_connection_commit_transport_handoff(
    TerminalConnection *connection) {
  if (!connection || !connection->operation || !connection->operation->handoff_prepared ||
      (connection->operation->pty || connection->operation->local_fd >= 0)) co_return nullptr;
  auto &operation = *connection->operation;
  operation.handoff_prepared = false;
  operation.handoff_committed = true;
  connection->state.phase = TerminalConnectionPhase::stopping;
  operation.io_stop.cancel();
  operation.stop.cancel();
  connection->wake();
  co_await terminal_connection_wait_stopped(connection);
  co_return connection->committed_origin.release();
}

std::shared_ptr<AuthenticatedSshTransport> terminal_transport_origin_authenticated_ssh(
    TerminalTransportOrigin *origin) {
  return origin && origin->transport.authenticated_ssh ? origin->transport.authenticated_ssh() : nullptr;
}

cardio::promise<void> terminal_transport_origin_wait(TerminalTransportOrigin *origin) {
  if (!origin) co_return;
  if (origin->inbound) co_await *origin->inbound;
  origin->stop.cancel();
  if (origin->outbound) co_await *origin->outbound;
  if (origin->controls) co_await *origin->controls;
  if (!origin->finished) {
    origin->finished = true;
    if (origin->transport.close) co_await origin->transport.close();
    origin->transport = {};
    if (origin->data_fd >= 0) ::close(std::exchange(origin->data_fd, -1));
    if (origin->control_fd >= 0) ::close(std::exchange(origin->control_fd, -1));
  }
}

cardio::promise<void> terminal_transport_origin_close(TerminalTransportOrigin *origin) {
  if (!origin) co_return;
  origin->stop.cancel();
  co_await terminal_transport_origin_wait(origin);
}

void destroy_terminal_transport_origin(TerminalTransportOrigin *origin) {
  delete origin;
}

TerminalLocalChildReaper *terminal_connection_commit_local_handoff(TerminalConnection *connection) {
  if (!connection || !connection->operation || !connection->operation->handoff_prepared) return nullptr;
  std::unique_ptr<TerminalLocalChildReaper> reaper;
  auto &operation = *connection->operation;
  if (operation.pty) {
    reaper = std::make_unique<TerminalLocalChildReaper>();
    reaper->child = connection->state.child;
    operation.handoff_reaper = reaper.get();
  }
  operation.handoff_prepared = false;
  operation.handoff_committed = true;
  connection->state.phase = TerminalConnectionPhase::stopping;
  operation.io_stop.cancel();
  operation.stop.cancel();
  connection->wake();
  return reaper.release();
}

cardio::promise<TerminalLocalChildExit> terminal_local_child_reaper_wait(TerminalLocalChildReaper *reaper) {
  while (!reaper->completion) co_await reaper->ready.wait();
  co_return co_await *reaper->completion;
}

void destroy_terminal_local_child_reaper(TerminalLocalChildReaper *reaper) {
  if (reaper && reaper->process_fd >= 0) ::close(reaper->process_fd);
  delete reaper;
}

TerminalConnectionProgress terminal_connection_dispatch(TerminalConnection *connection) {
  if (connection->operation && connection->operation->handoff_prepared) return {};
  if (connection->operation && connection->operation->finishing &&
      (!connection->operation->session || connection->operation->peer_closed)) {
    finish_input(connection);
    return {};
  }
  if (!connection->operation || !connection->operation->session)
    return {discard_outgoing(connection->terminal), false};
  if (!connection->operation || !connection->operation->session ||
      (connection->state.phase == TerminalConnectionPhase::stopping &&
       !connection->operation->finishing)) return {};
  auto &operation = *connection->operation;
  uint32_t ready = BOXRIG_SESSION_READY_NONE;
  if (operation.receive_delivery_enabled && (operation.incoming.length || operation.eof))
    ready |= BOXRIG_SESSION_READY_READ;
  if (operation.finishing || operation.outgoing.length < operation.outgoing.storage.size() || operation.write_closed)
    ready |= BOXRIG_SESSION_READY_WRITE;
  boxrig_session_dispatch_result result{};
  if (boxrig_session_dispatch(operation.session, ready, &result) != BOXRIG_SESSION_OK) {
    fail(connection, EIO);
    if (operation.finishing) {
      operation.peer_closed = true;
      operation.drained.trigger();
    }
    return {};
  }
  if (result.peer_closed) {
    operation.peer_closed = true;
    operation.drained.trigger();
  }
  const auto interests = boxrig_session_interests(operation.session);
  operation.stream_changed.trigger();
  return {result.terminal_bytes_fed != 0 || result.transport_bytes_written != 0 ||
      (operation.receive_delivery_enabled && (interests & BOXRIG_SESSION_INTEREST_READ) && operation.incoming.length != 0),
      result.terminal_bytes_fed != 0};
}

void terminal_connection_set_receive_delivery_enabled(TerminalConnection *connection, bool enabled) {
  if (connection->operation && connection->operation->receive_delivery_enabled != enabled) {
    connection->operation->receive_delivery_enabled = enabled;
    connection->wake();
  }
}

cardio::promise<bool> terminal_connection_resize(TerminalConnection *connection, std::size_t rows, std::size_t columns) {
  if (!connection->operation || !connection->operation->session || connection->operation->finishing)
    co_return boxrig_terminal_resize(connection->terminal, rows, columns) == BOXRIG_STATUS_OK;
  auto &operation = *connection->operation;
  ++operation.pending_operations;
  bool resized = false;
  try {
    const auto cancellation = operation.io_stop.get_cancellation();
    co_await operation.transport.resize(rows, columns, cancellation);
    cancellation.throw_if_cancellation_requested();
    resized = boxrig_terminal_resize(connection->terminal, rows, columns) == BOXRIG_STATUS_OK;
    if (!resized) fail(connection, EIO);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    fail(connection, error.code().value());
  } catch (...) {
    fail(connection, EIO);
  }
  --operation.pending_operations;
  operation.drained.trigger();
  co_return resized;
}

TerminalConnectionState terminal_connection_state(const TerminalConnection *connection) {
  auto state = connection->state;
  state.binary_receive = connection->operation && connection->operation->transfer_prepared;
  return state;
}

cardio::promise<bool> terminal_connection_change_encoding(TerminalConnection *connection, std::string encoding) {
  const bool starting = connection->state.phase == TerminalConnectionPhase::starting;
  if ((!starting && (connection->state.phase != TerminalConnectionPhase::connected ||
      !connection->state.carrier_connected)) ||
      connection->operation->changing_encoding || connection->operation->service_active) co_return false;
  if (connection->state.encoding == encoding) co_return true;
  TerminalTextStreamPtr decoder{nullptr, destroy_terminal_text_stream};
  TerminalTextStreamPtr encoder{nullptr, destroy_terminal_text_stream};
  try {
    decoder = create_terminal_text_decoder(encoding);
    encoder = create_terminal_text_encoder(encoding);
  } catch (const std::system_error &) {
    co_return false;
  }
  auto &operation = *connection->operation;
  if (starting) {
    // Open/spawn has not started either stream runner. Replace the unused
    // converters now so the first peer byte observes the latest preference.
    operation.codecs = {};
    operation.decoder = std::move(decoder);
    operation.encoder = std::move(encoder);
    connection->state.encoding = std::move(encoding);
    co_return true;
  }
  ++operation.pending_operations;
  operation.changing_encoding = true;
  operation.read_paused = true;
  operation.read_interrupt.cancel();
  operation.receive_space.trigger();
  const auto receiving = operation.io_stop.get_cancellation();
  auto stop = cardio::cancellations::any(receiving, operation.carrier_stop.get_cancellation());
  CancellationScope stop_scope{stop};
  const auto cancellation = stop.get_cancellation();
  bool changed = false;
  try {
    while (operation.read_active) co_await operation.stream_changed.wait(receiving);
    receiving.throw_if_cancellation_requested();
    if (operation.eof) throw cardio::canceled_exception();
    co_await queue_decoded(connection, co_await finish_auto_start_prefix(connection), receiving);
    co_await queue_pending_raw(connection, receiving);
    co_await queue_decoded(connection, co_await finish_decoder(connection), receiving);
    cancellation.throw_if_cancellation_requested();
    connection->wake();
    while (operation.write_active || operation.outgoing.length != 0 ||
        (boxrig_session_interests(operation.session) & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
      if (operation.write_closed)
        throw std::system_error(EPIPE, std::generic_category(), "encoding exchange on a closed writer");
      co_await operation.stream_changed.wait(cancellation);
    }
    operation.write_paused = true;
    auto tail = co_await finish_converter(operation.codecs.encoder, operation.encoder.get());
    if (!tail.empty()) co_await operation.transport.write(tail, cancellation);
    cancellation.throw_if_cancellation_requested();
    operation.codecs = {};
    operation.decoder = std::move(decoder);
    operation.encoder = std::move(encoder);
    operation.decoder_finished = false;
    connection->state.encoding = std::move(encoding);
    changed = true;
  } catch (const cardio::canceled_exception &) {
    if (!receiving.is_cancellation_requested() && operation.decoder_finished) {
      // The old receive boundary has already been delivered in full. Resume
      // the unchanged encoding with a fresh decoder after a carrier drop.
      operation.codecs.decoder.reset();
      operation.decoder = create_terminal_text_decoder(connection->state.encoding);
      operation.decoder_finished = false;
    }
  } catch (const std::system_error &error) {
    fail(connection, error.code().value());
  } catch (...) {
    fail(connection, EIO);
  }
  operation.changing_encoding = false;
  operation.write_paused = false;
  operation.read_paused = false;
  operation.read_resumed.trigger();
  --operation.pending_operations;
  operation.drained.trigger();
  connection->wake();
  co_return changed;
}

static cardio::promise<void> drain_output(TerminalConnection *connection, cardio::cancellation cancellation) {
  auto &operation = *connection->operation;
  connection->wake();
  while (operation.write_active || operation.outgoing.length != 0 ||
      (boxrig_session_interests(operation.session) & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
    if (operation.write_closed)
      throw std::system_error(EPIPE, std::generic_category(), "terminal writer is closed");
    co_await operation.stream_changed.wait(cancellation);
  }
  cancellation.throw_if_cancellation_requested();
}

bool terminal_connection_can_send_break(const TerminalConnection *connection) {
  return connection->state.phase == TerminalConnectionPhase::connected &&
      connection->state.carrier_connected &&
      !connection->operation->changing_encoding && !connection->operation->service_active &&
      static_cast<bool>(connection->operation->transport.send_break);
}

std::shared_ptr<AuthenticatedSshTransport> terminal_connection_authenticated_ssh(const TerminalConnection *connection) {
  if (connection->state.phase != TerminalConnectionPhase::connected ||
      !connection->operation->transport.authenticated_ssh) return {};
  return connection->operation->transport.authenticated_ssh();
}

cardio::promise<bool> terminal_connection_send_break(TerminalConnection *connection, TerminalBreakRequest request) {
  if (!terminal_connection_can_send_break(connection)) co_return false;
  auto &operation = *connection->operation;
  ++operation.pending_operations;
  operation.service_active = true;
  auto stop = cardio::cancellations::any(operation.io_stop.get_cancellation(), operation.carrier_stop.get_cancellation());
  CancellationScope stop_scope{stop};
  const auto cancellation = stop.get_cancellation();
  bool succeeded = false;
  try {
    co_await drain_output(connection, cancellation);
    operation.write_paused = true;
    co_await operation.transport.send_break(cancellation);
    succeeded = true;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (request.status) request.status(error.what());
  }
  operation.write_paused = false;
  operation.service_active = false;
  --operation.pending_operations;
  operation.drained.trigger();
  connection->wake();
  co_return succeeded;
}

cardio::promise<bool> terminal_connection_send_text(TerminalConnection *connection,
    TerminalTextSendRequest request, cardio::cancellation cancellation) {
  if (connection->state.phase != TerminalConnectionPhase::connected ||
      !connection->state.carrier_connected ||
      connection->operation->changing_encoding || connection->operation->service_active ||
      request.bytes_per_second == 0 || !terminal_text_send_source_is_valid(request.source)) co_return false;
  auto &operation = *connection->operation;
  ++operation.pending_operations;
  operation.service_active = true;
  auto stop = cardio::cancellations::any(cancellation, operation.io_stop.get_cancellation(), operation.carrier_stop.get_cancellation());
  CancellationScope stop_scope{stop};
  request.text_settings.encoding = connection->state.encoding;
  bool succeeded = false;
  try {
    co_await drain_output(connection, stop.get_cancellation());
    operation.write_paused = true;
    // A separate text-send encoder starts in its initial state. Finish the
    // normal writer's shift state before handing over the wire.
    auto tail = co_await finish_converter(operation.codecs.encoder, operation.encoder.get());
    operation.codecs.encoder.reset();
    operation.encoder = create_terminal_text_encoder(connection->state.encoding);
    if (!tail.empty()) co_await operation.transport.write(tail, stop.get_cancellation());
    TerminalTextSendTransport transport{
        [&operation](std::span<const unsigned char> bytes, cardio::cancellation token) {
          return operation.transport.write(bytes, token);
        },
        [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()); },
        [](std::uint64_t microseconds, cardio::cancellation token) {
          return cardio::promises::delay(microseconds / 1000 + (microseconds % 1000 != 0), token);
        }};
    co_await run_terminal_text_send_async(request, std::move(transport), stop.get_cancellation());
    succeeded = true;
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (request.status) request.status(error.what());
  }
  operation.write_paused = false;
  operation.service_active = false;
  --operation.pending_operations;
  operation.drained.trigger();
  connection->wake();
  co_return succeeded;
}

bool terminal_connection_prepare_transfer(TerminalConnection *connection) {
  if (connection->state.phase != TerminalConnectionPhase::connected ||
      !connection->state.carrier_connected ||
      connection->operation->changing_encoding || connection->operation->service_active ||
      connection->operation->transfer_incoming.length != 0 ||
      !connection->operation->transfer_prefetch.empty()) return false;
  auto &operation = *connection->operation;
  operation.service_active = true;
  operation.transfer_prepared = true;
  operation.transfer_carrier = operation.carrier_stop.get_cancellation();
  ++operation.pending_operations;
  operation.read_policy_changed = true;
  operation.read_interrupt.cancel();
  operation.receive_space.trigger();
  return true;
}

void terminal_connection_set_zmodem_auto_start(TerminalConnection *connection,
    std::function<bool(TerminalTransferDirection)> start) {
  connection->auto_start = std::move(start);
  if (connection->operation) {
    connection->operation->read_policy_changed = true;
    connection->operation->read_interrupt.cancel();
    connection->operation->receive_space.trigger();
  }
}

static cardio::promise<void> send_transfer_bytes(ConnectionOperation *operation,
    std::span<const std::uint8_t> bytes, std::uint32_t timeout_ms,
    std::size_t &written, cardio::cancellation cancellation) {
  written = 0;
  if (bytes.empty()) co_return;
  auto deadline = cardio::cancellations::timeout(timeout_ms);
  auto stop = cardio::cancellations::any(cancellation, operation->io_stop.get_cancellation(), deadline.get_cancellation());
  CancellationScope stop_scope{stop};
  try {
    // Binary frames bypass text conversion. Preserve their batches up to the
    // connection's byte bound instead of applying the converter's chunk size.
    const auto chunk = bytes.first(std::min(bytes.size(), operation->outgoing.storage.size()));
    co_await operation->transport.write(chunk, stop.get_cancellation());
    written = chunk.size();
  } catch (const cardio::canceled_exception &) {
    if (cancellation.is_cancellation_requested() || operation->io_stop.get_cancellation().is_cancellation_requested())
      throw xyzm_async_cancelled_error("transfer send cancelled");
    throw xyzm_async_timeout_error("transfer send timeout");
  }
}

static cardio::promise<void> receive_transfer_bytes(ConnectionOperation *operation,
    std::span<std::uint8_t> bytes, std::uint32_t timeout_ms,
    std::size_t &received, cardio::cancellation cancellation) {
  received = 0;
  if (bytes.empty()) co_return;
  cancellation.throw_if_cancellation_requested();
  operation->io_stop.get_cancellation().throw_if_cancellation_requested();
  if (operation->transfer_incoming.length == 0 && operation->transfer_prefetch.empty()) {
    if (operation->transport_eof) throw xyzm_async_io_error("transfer peer closed");
    if (timeout_ms == 0) throw xyzm_async_timeout_error("transfer receive is not ready");
    auto deadline = cardio::cancellations::timeout(timeout_ms);
    auto stop = cardio::cancellations::any(cancellation, operation->io_stop.get_cancellation(), deadline.get_cancellation());
    CancellationScope stop_scope{stop};
    try {
      while (operation->transfer_incoming.length == 0 && operation->transfer_prefetch.empty()) {
        if (operation->transport_eof) throw xyzm_async_io_error("transfer peer closed");
        co_await operation->transfer_ready.wait(stop.get_cancellation());
      }
    } catch (const cardio::canceled_exception &) {
      if (cancellation.is_cancellation_requested() || operation->io_stop.get_cancellation().is_cancellation_requested())
        throw xyzm_async_cancelled_error("transfer receive cancelled");
      throw xyzm_async_timeout_error("transfer receive timeout");
    }
  }
  if (!operation->transfer_prefetch.empty()) {
    received = std::min(bytes.size(), operation->transfer_prefetch.size() - operation->transfer_prefetch_offset);
    std::copy_n(operation->transfer_prefetch.data() + operation->transfer_prefetch_offset,
        received, bytes.begin());
    operation->transfer_prefetch_offset += received;
    if (operation->transfer_prefetch_offset == operation->transfer_prefetch.size()) {
      operation->transfer_prefetch.clear();
      operation->transfer_prefetch_offset = 0;
    }
    co_return;
  }
  // libxyzm returns its unconsumed read-ahead before transfer completion. The
  // parser can therefore receive a chunk without losing a following prompt.
  const auto available = front(operation->transfer_incoming);
  received = std::min(bytes.size(), available.size());
  std::copy_n(available.begin(), received, bytes.begin());
  consume(operation->transfer_incoming, received);
  operation->receive_space.trigger();
}

cardio::promise<bool> terminal_connection_transfer(TerminalConnection *connection,
    TerminalTransferRequest request, cardio::cancellation cancellation) {
  if (!connection->operation || !connection->operation->transfer_prepared) co_return false;
  auto &operation = *connection->operation;
  auto stop = cardio::cancellations::any(cancellation, operation.io_stop.get_cancellation(), operation.transfer_carrier);
  CancellationScope stop_scope{stop};
  const auto start_control = operation.transfer_start_control;
  operation.transfer_start_control.reset();
  bool succeeded = false;
  try {
    stop.get_cancellation().throw_if_cancellation_requested();
    if (connection->state.phase != TerminalConnectionPhase::connected)
      throw xyzm_async_io_error("transfer is not connected");
    if (request.direction == TerminalTransferDirection::send &&
        (request.protocol == TerminalTransferProtocol::xmodem ||
         request.protocol == TerminalTransferProtocol::ymodem) &&
        start_control &&
        std::chrono::steady_clock::now() - operation.transfer_start_control_time <= std::chrono::seconds(5)) {
      operation.transfer_prefetch.insert(operation.transfer_prefetch.begin(),
          static_cast<char>(*start_control));
    }
    co_await drain_output(connection, stop.get_cancellation());
    operation.write_paused = true;
    TerminalTransferTransport transport{
        [&operation](std::span<const std::uint8_t> bytes, std::uint32_t timeout,
            std::size_t &written, cardio::cancellation token) {
          return send_transfer_bytes(&operation, bytes, timeout, written, token);
        },
        [&operation](std::span<std::uint8_t> bytes, std::uint32_t timeout,
            std::size_t &received, cardio::cancellation token) {
          return receive_transfer_bytes(&operation, bytes, timeout, received, token);
        },
        [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()); },
        [&operation](std::span<const std::uint8_t> bytes) {
          // Returned cache bytes precede both unconsumed prefetch and later reads.
          operation.transfer_prefetch.erase(0, operation.transfer_prefetch_offset);
          operation.transfer_prefetch_offset = 0;
          if (!bytes.empty()) operation.transfer_prefetch.insert(0,
              reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }, operation.transport.buffer_ymodem_output};
    co_await run_terminal_transfer_async(request, std::move(transport), stop.get_cancellation());
    succeeded = true;
  } catch (const cardio::canceled_exception &) {
  } catch (const xyzm_async_cancelled_error &) {
  } catch (const std::exception &error) {
    if (request.status) request.status(error.what());
  }
  // Join the old read boundary before returning its unconsumed bytes to text
  // decoding. Cancellation of this transfer does not cancel the connection.
  operation.read_paused = true;
  operation.read_interrupt.cancel();
  operation.receive_space.trigger();
  while (operation.read_active) co_await operation.stream_changed.wait();
  if (!succeeded) {
    consume(operation.transfer_incoming, operation.transfer_incoming.length);
    operation.transfer_prefetch.clear();
    operation.transfer_prefetch_offset = 0;
  }
  operation.transfer_prepared = false;
  operation.service_active = false;
  operation.write_paused = false;
  operation.read_paused = false;
  operation.read_resumed.trigger();
  --operation.pending_operations;
  operation.drained.trigger();
  connection->wake();
  co_return succeeded;
}

}
