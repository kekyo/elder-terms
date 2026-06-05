#include "terminal-transfer-runner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

#include <gio/gio.h>
#include <glib.h>

namespace elder_terms {

static constexpr std::uint32_t transfer_handshake_timeout_ms = 5000;
static constexpr std::uint32_t transfer_block_timeout_ms = 5000;
static constexpr std::uint32_t transfer_retry_limit = 8;
static constexpr std::uint8_t transfer_pad_byte = 0x1a;
static constexpr const char *fallback_receive_name = "received.bin";

struct GObjectDeleter {
  void operator()(void *object) const {
    if (object != nullptr) {
      g_object_unref(object);
    }
  }
};

struct GFreeDeleter {
  void operator()(void *value) const {
    g_free(value);
  }
};

template <typename T> using GObjectPtr = std::unique_ptr<T, GObjectDeleter>;
using GCharPtr = std::unique_ptr<char, GFreeDeleter>;

struct TransferProgressState {
  TerminalTransferStatusCallback status;
  TerminalTransferDirection direction = TerminalTransferDirection::send;
  std::string file_name = fallback_receive_name;
  std::optional<std::uint64_t> total_bytes;
  std::uint64_t transferred_bytes = 0;

  void publish() const {
    if (status) {
      status(format_transfer_status(file_name, transferred_bytes,
                                    total_bytes));
    }
  }
};

struct SendSourceState {
  std::vector<std::string> source_file_uris;
  std::size_t next_index = 0;
  GObjectPtr<GFile> current_file;
  GObjectPtr<GFileInputStream> current_stream;
  xyzm_async_file_info_t current_info;
  TransferProgressState *progress = nullptr;
};

struct ReceiveSinkState {
  TerminalTransferProtocol protocol = TerminalTransferProtocol::zmodem;
  GObjectPtr<GFile> base_directory;
  GObjectPtr<GFile> current_target_file;
  GObjectPtr<GFile> current_partial_file;
  GObjectPtr<GFileOutputStream> current_stream;
  TransferProgressState *progress = nullptr;
};

static std::span<std::byte> writable_bytes(std::span<std::uint8_t> bytes) {
  return std::span<std::byte>(reinterpret_cast<std::byte *>(bytes.data()),
                              bytes.size());
}

static std::span<const std::byte>
readonly_bytes(std::span<const std::uint8_t> bytes) {
  return std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
}

static std::string g_file_basename(GFile *file) {
  GCharPtr basename(g_file_get_basename(file));
  if (basename == nullptr || basename.get()[0] == '\0') {
    return fallback_receive_name;
  }
  return basename.get();
}

static bool has_uri_scheme(const std::string &text) {
  if (text.empty()) {
    return false;
  }

  GCharPtr scheme(g_uri_parse_scheme(text.c_str()));
  return scheme != nullptr && scheme.get()[0] != '\0';
}

static GObjectPtr<GFile> make_file_from_path_or_uri(const std::string &text) {
  if (text.empty()) {
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (downloads != nullptr && downloads[0] != '\0') {
      return GObjectPtr<GFile>(g_file_new_for_path(downloads));
    }

    GCharPtr current_directory(g_get_current_dir());
    return GObjectPtr<GFile>(g_file_new_for_path(current_directory.get()));
  }

  if (has_uri_scheme(text)) {
    return GObjectPtr<GFile>(g_file_new_for_uri(text.c_str()));
  }
  return GObjectPtr<GFile>(g_file_new_for_path(text.c_str()));
}

std::string resolve_transfer_base_path_uri(const std::string &base_path) {
  GObjectPtr<GFile> file = make_file_from_path_or_uri(base_path);
  GCharPtr uri(g_file_get_uri(file.get()));
  return uri == nullptr ? std::string() : std::string(uri.get());
}

static GObjectPtr<GFile> child_file(GFile *directory,
                                    const std::string &name) {
  return GObjectPtr<GFile>(g_file_get_child(directory, name.c_str()));
}

static std::optional<std::uint64_t> file_info_size(GFileInfo *info) {
  if (info == nullptr ||
      !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(g_file_info_get_size(info));
}

static void set_file_info_metadata(xyzm_async_file_info_t *info,
                                   GFile *file, GFileInfo *file_info) {
  info->name = sanitize_transfer_file_name(
      g_file_info_has_attribute(file_info, G_FILE_ATTRIBUTE_STANDARD_NAME)
          ? g_file_info_get_name(file_info)
          : g_file_basename(file),
      fallback_receive_name);
  info->size_bytes = file_info_size(file_info);
  if (g_file_info_has_attribute(file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
    info->mtime_unix_seconds = static_cast<std::uint64_t>(
        g_file_info_get_attribute_uint64(file_info,
                                         G_FILE_ATTRIBUTE_TIME_MODIFIED));
  }
  if (g_file_info_has_attribute(file_info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
    info->mode = static_cast<std::uint32_t>(
        g_file_info_get_attribute_uint32(file_info,
                                         G_FILE_ATTRIBUTE_UNIX_MODE));
  }
}

static std::string error_message(std::exception_ptr error) {
  if (!error) {
    return {};
  }

  try {
    std::rethrow_exception(error);
  } catch (const std::exception &exception) {
    return exception.what();
  } catch (...) {
    return "unknown error";
  }
}

static cardio::promise<void>
move_file_async(GFile *source, GFile *destination,
                cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [source, destination](GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data) {
        g_file_move_async(source, destination, G_FILE_COPY_OVERWRITE,
                          G_PRIORITY_DEFAULT, cancellable, nullptr, nullptr,
                          callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_file_move_finish(G_FILE(object), result, error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
  co_return;
}

static cardio::promise<std::optional<std::uint64_t>>
query_existing_size_async(GFile *file, cardio::cancellation cancellation) {
  try {
    cancellation.throw_if_cancellation_requested();
    GObjectPtr<GFileInfo> info(co_await cardio::gio::query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT));
    cancellation.throw_if_cancellation_requested();
    co_return file_info_size(info.get());
  } catch (const cardio::gio::gio_error &error) {
    if (error.domain() == G_IO_ERROR &&
        error.code() == G_IO_ERROR_NOT_FOUND) {
      co_return std::nullopt;
    }
    throw;
  }
}

static std::string format_byte_count(std::uint64_t bytes) {
  static constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit_index = 0;
  while (value >= 1024.0 && unit_index + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit_index;
  }

  char buffer[64];
  const double rounded = std::round(value);
  if (unit_index == 0 ||
      value >= 10.0 ||
      std::abs(value - rounded) < 0.05) {
    std::snprintf(buffer, sizeof(buffer), "%.0f%s", value, units[unit_index]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f%s", value, units[unit_index]);
  }
  return buffer;
}

static std::uint64_t clamped_percent(std::uint64_t transferred,
                                     std::uint64_t total) {
  if (total == 0) {
    return 0;
  }
  const std::uint64_t percent = transferred > total
                                    ? 100
                                    : (transferred * 100) / total;
  return percent > 100 ? 100 : percent;
}

std::string sanitize_transfer_file_name(const std::string &name,
                                        const std::string &fallback) {
  const std::size_t separator = name.find_last_of("/\\");
  std::string result =
      separator == std::string::npos ? name : name.substr(separator + 1);
  for (char &character : result) {
    const unsigned char value = static_cast<unsigned char>(character);
    if (value < 0x20 || character == '/' || character == '\\') {
      character = '_';
    }
  }

  if (result.empty() || result == "." || result == "..") {
    return fallback.empty() ? fallback_receive_name : fallback;
  }
  return result;
}

std::string format_transfer_status(
    const std::string &file_name, std::uint64_t transferred_bytes,
    std::optional<std::uint64_t> total_bytes) {
  std::string result = file_name.empty() ? fallback_receive_name : file_name;
  result += " ";
  result += format_byte_count(transferred_bytes);
  if (total_bytes.has_value()) {
    result += "/";
    result += format_byte_count(*total_bytes);
    result += " (";
    result += std::to_string(clamped_percent(transferred_bytes, *total_bytes));
    result += "%)";
  }
  return result;
}

static void update_progress_file(TransferProgressState *state,
                                 const xyzm_async_file_info_t &info) {
  if (state == nullptr) {
    return;
  }

  const std::string next_name = sanitize_transfer_file_name(
      info.name.value_or(fallback_receive_name), fallback_receive_name);
  if (state->direction != TerminalTransferDirection::receive ||
      state->file_name != next_name) {
    state->transferred_bytes = 0;
  }
  state->file_name = next_name;
  state->total_bytes = info.size_bytes;
  state->publish();
}

static void update_progress_delta(TransferProgressState *state,
                                  std::uint64_t payload_delta) {
  if (state == nullptr || payload_delta == 0) {
    return;
  }

  state->transferred_bytes += payload_delta;
  state->publish();
}

static xyzm_async_transfer_observer_t
make_transfer_observer(TransferProgressState *progress) {
  xyzm_async_transfer_observer_t observer;
  observer.notify = [progress](const xyzm_async_transfer_event_t &event) {
    if (event.kind == XYZM_TRANSFER_EVENT_FILE_STARTED &&
        event.file_info.has_value()) {
      update_progress_file(progress, event.file_info->get());
      return;
    }
    if (event.kind == XYZM_TRANSFER_EVENT_PROGRESS) {
      update_progress_delta(progress, event.payload_delta);
    }
  };
  return observer;
}

static xyzm_async_link_ops_t
make_link_ops(TerminalTransferTransport *transport) {
  xyzm_async_link_ops_t link;
  link.send = [transport](std::span<const std::uint8_t> bytes,
                          std::uint32_t timeout_ms, std::size_t &written_len,
                          cardio::cancellation cancellation)
      -> cardio::promise<void> {
    co_await transport->send(bytes, timeout_ms, written_len,
                             std::move(cancellation));
    co_return;
  };
  link.recv = [transport](std::span<std::uint8_t> bytes,
                          std::uint32_t timeout_ms, std::size_t &read_len,
                          cardio::cancellation cancellation)
      -> cardio::promise<void> {
    co_await transport->recv(bytes, timeout_ms, read_len,
                             std::move(cancellation));
    co_return;
  };
  link.now_ms = [transport]() { return transport->now_ms(); };
  return link;
}

static xyzm_async_source_ops_t make_source_ops(SendSourceState *state) {
  xyzm_async_source_ops_t source;
  source.next = [state](xyzm_async_file_info_t &info,
                        cardio::cancellation cancellation)
      -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    state->current_stream.reset();
    state->current_file.reset();
    state->current_info = {};

    if (state->next_index >= state->source_file_uris.size()) {
      co_return xyzm_async_status_t::end;
    }

    const std::string uri = state->source_file_uris[state->next_index++];
    state->current_file = make_file_from_path_or_uri(uri);
    GObjectPtr<GFileInfo> file_info(co_await cardio::gio::query_info(
        state->current_file.get(),
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED ","
        G_FILE_ATTRIBUTE_UNIX_MODE,
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT));
    set_file_info_metadata(&state->current_info, state->current_file.get(),
                           file_info.get());
    state->current_stream =
        GObjectPtr<GFileInputStream>(co_await cardio::gio::read(
            state->current_file.get(), G_PRIORITY_DEFAULT));
    info = state->current_info;
    update_progress_file(state->progress, info);
    co_return xyzm_async_status_t::ok;
  };
  source.read =
      [state](std::span<std::uint8_t> bytes, std::size_t &read_len,
              cardio::cancellation cancellation)
          -> cardio::promise<xyzm_async_status_t> {
    cancellation.throw_if_cancellation_requested();
    if (state->current_stream == nullptr) {
      throw xyzm_async_io_error("source file is not open");
    }

    read_len = co_await cardio::gio::read(
        G_INPUT_STREAM(state->current_stream.get()), writable_bytes(bytes),
        std::move(cancellation), G_PRIORITY_DEFAULT);
    if (read_len == 0) {
      co_return xyzm_async_status_t::end;
    }
    co_return xyzm_async_status_t::ok;
  };
  source.seek = [state](std::uint64_t offset,
                        cardio::cancellation cancellation)
      -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    if (state->current_stream == nullptr ||
        !G_IS_SEEKABLE(state->current_stream.get())) {
      throw xyzm_async_unsupported_error("source file cannot seek");
    }

    GError *error = nullptr;
    if (!g_seekable_seek(G_SEEKABLE(state->current_stream.get()),
                         static_cast<goffset>(offset), G_SEEK_SET, nullptr,
                         &error)) {
      const std::string message =
          error != nullptr && error->message != nullptr
              ? error->message
              : "source seek failed";
      g_clear_error(&error);
      throw xyzm_async_io_error(message);
    }
    if (state->progress != nullptr) {
      state->progress->transferred_bytes = offset;
      state->progress->publish();
    }
    co_return;
  };
  source.end = [state](const xyzm_async_file_info_t &, std::exception_ptr,
                       cardio::cancellation cancellation)
      -> cardio::promise<void> {
    if (state->current_stream != nullptr) {
      co_await cardio::gio::close(G_INPUT_STREAM(state->current_stream.get()),
                                  G_PRIORITY_DEFAULT);
    }
    cancellation.throw_if_cancellation_requested();
    state->current_stream.reset();
    state->current_file.reset();
    state->current_info = {};
    co_return;
  };
  return source;
}

static xyzm_async_sink_ops_t make_sink_ops(ReceiveSinkState *state) {
  xyzm_async_sink_ops_t sink;
  sink.begin = [state](const xyzm_async_file_info_t &info,
                       std::uint64_t &resume_offset,
                       cardio::cancellation cancellation)
      -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    state->current_stream.reset();
    state->current_target_file.reset();
    state->current_partial_file.reset();

    const bool allow_resume =
        state->protocol == TerminalTransferProtocol::zmodem;
    const std::string file_name = sanitize_transfer_file_name(
        info.name.value_or(fallback_receive_name), fallback_receive_name);
    state->current_target_file = child_file(state->base_directory.get(),
                                            file_name);
    state->current_partial_file = child_file(state->base_directory.get(),
                                             file_name + ".partial");

    const std::optional<std::uint64_t> existing_size =
        allow_resume ? co_await query_existing_size_async(
                           state->current_partial_file.get(), cancellation)
                     : std::nullopt;
    resume_offset = existing_size.value_or(0);
    if (resume_offset > 0) {
      state->current_stream =
          GObjectPtr<GFileOutputStream>(co_await cardio::gio::append_to(
              state->current_partial_file.get(), G_FILE_CREATE_NONE,
              G_PRIORITY_DEFAULT));
    } else {
      state->current_stream =
          GObjectPtr<GFileOutputStream>(co_await cardio::gio::replace(
              state->current_partial_file.get(), nullptr, false,
              G_FILE_CREATE_NONE, G_PRIORITY_DEFAULT));
    }

    if (state->progress != nullptr) {
      state->progress->file_name = file_name;
      state->progress->total_bytes = info.size_bytes;
      state->progress->transferred_bytes = resume_offset;
      state->progress->publish();
    }
    co_return;
  };
  sink.write =
      [state](std::span<const std::uint8_t> bytes,
              cardio::cancellation cancellation) -> cardio::promise<void> {
    cancellation.throw_if_cancellation_requested();
    if (state->current_stream == nullptr) {
      throw xyzm_async_io_error("sink file is not open");
    }

    std::span<const std::byte> remaining = readonly_bytes(bytes);
    while (!remaining.empty()) {
      const std::size_t written = co_await cardio::gio::write(
          G_OUTPUT_STREAM(state->current_stream.get()), remaining,
          cancellation, G_PRIORITY_DEFAULT);
      if (written == 0 || written > remaining.size()) {
        throw xyzm_async_io_error("sink write was incomplete");
      }
      remaining = remaining.subspan(written);
      cancellation.throw_if_cancellation_requested();
    }
    co_return;
  };
  sink.end = [state](const xyzm_async_file_info_t &, std::exception_ptr error,
                     cardio::cancellation cancellation)
      -> cardio::promise<void> {
    if (state->current_stream != nullptr) {
      co_await cardio::gio::close(G_OUTPUT_STREAM(state->current_stream.get()),
                                  G_PRIORITY_DEFAULT);
      state->current_stream.reset();
    }

    if (!error && state->current_partial_file != nullptr &&
        state->current_target_file != nullptr) {
      co_await move_file_async(state->current_partial_file.get(),
                               state->current_target_file.get(),
                               std::move(cancellation));
    } else {
      const std::string message = error_message(error);
      if (!message.empty()) {
        std::cerr << "Warning: transfer file failed: " << message << '\n';
      }
      cancellation.throw_if_cancellation_requested();
    }
    state->current_partial_file.reset();
    state->current_target_file.reset();
    co_return;
  };
  return sink;
}

static xyzm_xmodem_send_opts_t default_xmodem_send_opts() {
  return {
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .checksum_mode = XYZM_XMODEM_CHECKSUM_MODE_AUTO,
      .packet_size = XYZM_XMODEM_PACKET_SIZE_1K,
  };
}

static xyzm_xmodem_receive_opts_t default_xmodem_receive_opts() {
  return {
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .checksum_mode = XYZM_XMODEM_CHECKSUM_MODE_CRC,
  };
}

static xyzm_ymodem_opts_t default_ymodem_send_opts() {
  return {
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .variant = XYZM_YMODEM_VARIANT_AUTO,
  };
}

static xyzm_ymodem_opts_t default_ymodem_receive_opts() {
  return {
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .variant = XYZM_YMODEM_VARIANT_STANDARD,
  };
}

static xyzm_zmodem_opts_t default_zmodem_opts() {
  return {
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .zmodem_packet_len = 0,
      .zmodem_escape_ctrl = 1,
      .zmodem_use_crc32 = 1,
  };
}

static cardio::promise<void>
run_send_async(const TerminalTransferRequest &request,
               xyzm_async_link_ops_t *link,
               xyzm_async_transfer_observer_t *observer,
               xyzm_async_transfer_report_t *report,
               SendSourceState *source_state,
               cardio::cancellation cancellation) {
  xyzm_async_source_ops_t source = make_source_ops(source_state);
  if (request.protocol == TerminalTransferProtocol::xmodem) {
    xyzm_xmodem_send_opts_t options = default_xmodem_send_opts();
    xyzm_xmodem_send_async_request_t lib_request{link, &source, &options,
                                                 observer};
    co_await xyzm_xmodem_send_async(&lib_request, report, cancellation);
    co_return;
  }
  if (request.protocol == TerminalTransferProtocol::ymodem) {
    xyzm_ymodem_opts_t options = default_ymodem_send_opts();
    xyzm_ymodem_send_async_request_t lib_request{link, &source, &options,
                                                 observer};
    co_await xyzm_ymodem_send_batch_async(&lib_request, report,
                                          cancellation);
    co_return;
  }

  xyzm_zmodem_opts_t options = default_zmodem_opts();
  xyzm_zmodem_send_async_request_t lib_request{link, &source, &options,
                                               observer};
  co_await xyzm_zmodem_send_batch_async(&lib_request, report, cancellation);
}

static cardio::promise<void>
run_receive_async(const TerminalTransferRequest &request,
                  xyzm_async_link_ops_t *link,
                  xyzm_async_transfer_observer_t *observer,
                  xyzm_async_transfer_report_t *report,
                  ReceiveSinkState *sink_state,
                  cardio::cancellation cancellation) {
  xyzm_async_sink_ops_t sink = make_sink_ops(sink_state);
  if (request.protocol == TerminalTransferProtocol::xmodem) {
    xyzm_xmodem_receive_opts_t options = default_xmodem_receive_opts();
    xyzm_xmodem_receive_async_request_t lib_request{link, &sink, &options,
                                                    observer};
    co_await xyzm_xmodem_receive_async(&lib_request, report, cancellation);
    co_return;
  }
  if (request.protocol == TerminalTransferProtocol::ymodem) {
    xyzm_ymodem_opts_t options = default_ymodem_receive_opts();
    xyzm_ymodem_receive_async_request_t lib_request{link, &sink, &options,
                                                    observer};
    co_await xyzm_ymodem_receive_batch_async(&lib_request, report,
                                             cancellation);
    co_return;
  }

  xyzm_zmodem_opts_t options = default_zmodem_opts();
  xyzm_zmodem_receive_async_request_t lib_request{link, &sink, &options,
                                                  observer};
  co_await xyzm_zmodem_receive_batch_async(&lib_request, report,
                                           cancellation);
}

cardio::promise<void>
run_terminal_transfer_async(TerminalTransferRequest request,
                            TerminalTransferTransport transport,
                            cardio::cancellation cancellation) {
  if (!transport.send || !transport.recv || !transport.now_ms) {
    throw xyzm_async_argument_error("missing transfer transport callback");
  }
  if (request.direction == TerminalTransferDirection::send &&
      request.source_file_uris.empty()) {
    throw xyzm_async_argument_error("missing transfer source file");
  }

  TransferProgressState progress{
      .status = request.status,
      .direction = request.direction,
      .file_name = fallback_receive_name,
      .total_bytes = std::nullopt,
      .transferred_bytes = 0,
  };
  if (request.status) {
    request.status(std::string("Starting ") +
                   terminal_transfer_protocol_label(request.protocol) +
                   (request.direction == TerminalTransferDirection::send
                        ? " send"
                        : " receive"));
  }

  xyzm_async_link_ops_t link = make_link_ops(&transport);
  xyzm_async_transfer_observer_t observer = make_transfer_observer(&progress);
  xyzm_async_transfer_report_t report{};

  if (request.direction == TerminalTransferDirection::send) {
    SendSourceState source_state{
        .source_file_uris = request.source_file_uris,
        .next_index = 0,
        .current_file = nullptr,
        .current_stream = nullptr,
        .current_info = {},
        .progress = &progress,
    };
    co_await run_send_async(request, &link, &observer, &report, &source_state,
                            cancellation);
    co_return;
  }

  ReceiveSinkState sink_state{
      .protocol = request.protocol,
      .base_directory = make_file_from_path_or_uri(request.base_path),
      .current_target_file = nullptr,
      .current_partial_file = nullptr,
      .current_stream = nullptr,
      .progress = &progress,
  };
  co_await run_receive_async(request, &link, &observer, &report, &sink_state,
                             cancellation);
}

} // namespace elder_terms
