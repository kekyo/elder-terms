#include "terminal-text-send-runner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gio/gio.h>

#include "terminal-sessions/terminal-text-codec.h"

namespace elder_terms {

static constexpr std::size_t text_source_read_size = 64 * 1024;
static constexpr std::size_t maximum_text_send_chunk_size = 64 * 1024;
static constexpr std::uint64_t throttle_intervals_per_second = 10;
static constexpr std::uint64_t microseconds_per_second = 1000000;
static constexpr char replacement_warning[] =
    "Text contained characters that were replaced";

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

static std::optional<std::uint64_t> source_size(GFileInfo *info) {
  if (info == nullptr ||
      !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(g_file_info_get_size(info));
}

static std::size_t text_send_chunk_size(std::uint64_t bytes_per_second) {
  const std::uint64_t interval_bytes =
      std::max<std::uint64_t>(1,
                              bytes_per_second /
                                  throttle_intervals_per_second);
  return static_cast<std::size_t>(std::min<std::uint64_t>(
      interval_bytes, maximum_text_send_chunk_size));
}

static std::uint64_t payload_duration_us(std::size_t size,
                                         std::uint64_t bytes_per_second) {
  const long double duration =
      std::ceil((static_cast<long double>(size) *
                 static_cast<long double>(microseconds_per_second)) /
                static_cast<long double>(bytes_per_second));
  if (duration >=
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(duration);
}

static std::uint64_t saturating_add(std::uint64_t left,
                                    std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

static void publish_progress(const TerminalTextSendRequest &request,
                             std::uint64_t consumed,
                             std::optional<std::uint64_t> total) {
  if (!request.progress) {
    return;
  }
  if (!total.has_value()) {
    request.progress(TerminalTransferProgress{
        .mode = TerminalTransferProgressMode::indeterminate,
        .fraction = std::nullopt,
    });
    return;
  }
  const double fraction =
      *total == 0
          ? 1.0
          : std::clamp(static_cast<double>(consumed) /
                           static_cast<double>(*total),
                       0.0, 1.0);
  request.progress(TerminalTransferProgress{
      .mode = TerminalTransferProgressMode::determinate,
      .fraction = fraction,
  });
}

static void publish_replacement_warning(const TerminalTextSendRequest &request,
                                        bool *warning_published,
                                        bool used_replacement) {
  if (!used_replacement || *warning_published) {
    return;
  }
  *warning_published = true;
  if (request.status) {
    request.status(replacement_warning);
  }
}

static cardio::promise<void> send_encoded_bytes(
    std::vector<unsigned char> bytes, const TerminalTextSendRequest &request,
    TerminalTextSendTransport *transport, std::uint64_t *next_send_us,
    bool *has_send_deadline, cardio::cancellation cancellation) {
  const std::size_t chunk_size = text_send_chunk_size(request.bytes_per_second);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    cancellation.throw_if_cancellation_requested();
    std::uint64_t now = transport->now_us();
    if (*has_send_deadline && now < *next_send_us) {
      co_await transport->delay(*next_send_us - now, cancellation);
      cancellation.throw_if_cancellation_requested();
      now = transport->now_us();
    }

    const std::size_t size = std::min(chunk_size, bytes.size() - offset);
    const std::uint64_t send_started_us = now;
    co_await transport->send(
        std::span<const unsigned char>(bytes.data() + offset, size),
        cancellation);
    offset += size;

    const std::uint64_t interval_started_us =
        *has_send_deadline
            ? std::max(*next_send_us, send_started_us)
            : send_started_us;
    *next_send_us = saturating_add(
        interval_started_us,
        payload_duration_us(size, request.bytes_per_second));
    *has_send_deadline = true;
  }
}

cardio::promise<void>
run_terminal_text_send_async(TerminalTextSendRequest request,
                             TerminalTextSendTransport transport,
                             cardio::cancellation cancellation) {
  if (request.source_file_uri.empty()) {
    throw std::invalid_argument("text send source URI is empty");
  }
  if (request.bytes_per_second == 0) {
    throw std::invalid_argument("text send rate must be positive");
  }
  if (!transport.send || !transport.now_us || !transport.delay) {
    throw std::invalid_argument("text send transport is incomplete");
  }

  cancellation.throw_if_cancellation_requested();
  GObjectPtr<GFile> file(g_file_new_for_uri(request.source_file_uri.c_str()));
  GObjectPtr<GFileInfo> info(co_await cardio::gio::query_info(
      file.get(),
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT));
  cancellation.throw_if_cancellation_requested();
  const std::optional<std::uint64_t> total_size = source_size(info.get());
  GObjectPtr<GFileInputStream> stream(
      co_await cardio::gio::read(file.get(), G_PRIORITY_DEFAULT));
  cancellation.throw_if_cancellation_requested();

  TerminalTextEncoder encoder(request.text_settings);
  std::array<std::byte, text_source_read_size> buffer{};
  std::uint64_t consumed = 0;
  std::uint64_t next_send_us = 0;
  bool has_send_deadline = false;
  bool warning_published = false;
  publish_progress(request, consumed, total_size);

  while (true) {
    const std::size_t read_size = co_await cardio::gio::read(
        G_INPUT_STREAM(stream.get()),
        std::span<std::byte>(buffer.data(), buffer.size()), cancellation,
        G_PRIORITY_DEFAULT);
    if (read_size == 0) {
      break;
    }
    cancellation.throw_if_cancellation_requested();

    const auto *input =
        reinterpret_cast<const unsigned char *>(buffer.data());
    TerminalTextConversionResult converted = encoder.encode(
        std::span<const unsigned char>(input, read_size));
    publish_replacement_warning(request, &warning_published,
                                converted.used_replacement);
    co_await send_encoded_bytes(std::move(converted.bytes), request, &transport,
                                &next_send_us, &has_send_deadline,
                                cancellation);
    consumed += read_size;
    publish_progress(request, consumed, total_size);
  }

  TerminalTextConversionResult finished = encoder.finish();
  publish_replacement_warning(request, &warning_published,
                              finished.used_replacement);
  co_await send_encoded_bytes(std::move(finished.bytes), request, &transport,
                              &next_send_us, &has_send_deadline,
                              cancellation);
  publish_progress(request, consumed, total_size);
  co_return;
}

} // namespace elder_terms
