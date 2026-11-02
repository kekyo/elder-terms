// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "webdav-stream.h"
#include "../http/curl-http-stream.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace elder_terms {

cardio::promise<std::unique_ptr<RemoteFileReader>> open_webdav_reader_async(
    std::shared_ptr<CurlHttpSession> session, WebdavEndpoint endpoint,
    std::string path, cardio::cancellation cancellation) {
  HttpReadOptions options;
  options.url = webdav_resource_url(endpoint, webdav_virtual_path(std::move(path)));
  options.redirect = [endpoint](const std::string &url, const std::string &location) {
    const auto path = webdav_reference_path(endpoint, url, location);
    auto target = webdav_resource_url(endpoint, path);
    if (location.ends_with('/') && !target.ends_with('/')) target += '/';
    return target;
  };
  co_return std::move(co_await open_http_reader_async(std::move(session), std::move(options), cancellation));
}

struct WebdavWriteState {
  std::shared_ptr<CurlHttpSession> session;
  std::string url;
  cardio::cancellation_source stopping;
  cardio::cancellation parent_cancellation;
  std::array<std::byte, 256 * 1024> bytes{};
  std::size_t head = 0;
  std::size_t count = 0;
  std::uint64_t expected = 0;
  std::uint64_t accepted = 0;
  std::uint64_t sent = 0;
  bool paused = false;
  bool finished = false;
  bool closed = false;
  RemoteFileCreationState creation = RemoteFileCreationState::unknown;
  std::exception_ptr failure;
  std::shared_ptr<cardio::promise_source<void>> waiting;
};

static void wake_writer(WebdavWriteState &state) {
  auto waiting = std::exchange(state.waiting, {});
  if (waiting) waiting->try_resolve();
}

static cardio::promise<void> wait_writer_async(
    std::shared_ptr<WebdavWriteState> state, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (state->waiting) throw std::logic_error("Concurrent operations on one WebDAV writer");
  auto source = std::make_shared<cardio::promise_source<void>>();
  auto pending = source->get_promise();
  state->waiting = source;
  auto registration = cancellation.on_cancellation_requested([source] { source->try_cancel(); });
  std::exception_ptr failure;
  try { co_await pending; } catch (...) { failure = std::current_exception(); }
  if (state->waiting == source) state->waiting.reset();
  if (failure) std::rethrow_exception(failure);
}

static std::size_t send_stream_chunk(WebdavWriteState &state, std::span<std::byte> output) {
  if (!state.count) {
    if (state.sent == state.expected) return 0;
    state.paused = true;
    return CURL_READFUNC_PAUSE;
  }
  const auto count = std::min(output.size(), state.count);
  const auto first = std::min(count, state.bytes.size() - state.head);
  std::copy_n(state.bytes.begin() + state.head, first, output.begin());
  std::copy_n(state.bytes.begin(), count - first, output.begin() + first);
  state.head = (state.head + count) % state.bytes.size();
  state.count -= count;
  state.sent += count;
  wake_writer(state);
  return count;
}

static cardio::promise<void> send_webdav_request_async(std::shared_ptr<WebdavWriteState> state) {
  try {
    auto combined = cardio::cancellations::any(state->parent_cancellation, state->stopping.get_cancellation());
    CurlHttpRequest request;
    request.method = "PUT";
    request.url = state->url;
    request.headers = {"If-None-Match: *", "Content-Type: application/octet-stream"};
    request.upload_size = state->expected;
    request.send = [state](std::span<std::byte> output) { return send_stream_chunk(*state, output); };
    const auto result = co_await perform_http_request_async(state->session, std::move(request), combined.get_cancellation());
    if (result.code == CURLE_OK && result.status >= 300 && result.status < 400)
      throw std::runtime_error("WebDAV PUT redirect was not followed; check the configured resource path");
    if (result.code == CURLE_OK && result.status == 412)
      state->creation = RemoteFileCreationState::not_created;
    if (result.code != CURLE_OK || result.status != 201) throw http_error(result);
    // Only a completed successful exclusive PUT proves ownership. A missing
    // final response cannot justify deleting a path that might be foreign.
    state->creation = RemoteFileCreationState::created;
    if (state->sent != state->expected)
      throw std::runtime_error("WebDAV accepted an upload before receiving its complete declared body");
  } catch (...) { state->failure = std::current_exception(); }
  state->finished = true;
  wake_writer(*state);
}

static cardio::promise<void> write_webdav_stream_async(
    std::shared_ptr<WebdavWriteState> state, std::span<const std::byte> input,
    cardio::cancellation cancellation) {
  std::exception_ptr failure;
  try {
    cancellation.throw_if_cancellation_requested();
    state->parent_cancellation.throw_if_cancellation_requested();
    if (state->closed) throw std::runtime_error("WebDAV writer is closed");
    if (input.size() > state->expected - state->accepted)
      throw std::runtime_error("WebDAV upload source grew beyond its declared size");
    while (!input.empty()) {
      cancellation.throw_if_cancellation_requested();
      state->parent_cancellation.throw_if_cancellation_requested();
      if (state->failure) std::rethrow_exception(state->failure);
      if (state->finished) throw std::runtime_error("WebDAV upload ended before accepting all input");
      const auto count = std::min(input.size(), state->bytes.size() - state->count);
      if (!count) { co_await wait_writer_async(state, cancellation); continue; }
      const auto tail = (state->head + state->count) % state->bytes.size();
      const auto first = std::min(count, state->bytes.size() - tail);
      std::copy_n(input.begin(), first, state->bytes.begin() + tail);
      std::copy_n(input.begin() + first, count - first, state->bytes.begin());
      state->count += count;
      state->accepted += count;
      input = input.subspan(count);
      if (state->paused) {
        state->paused = false;
        resume_curl_http_session(state->session);
      }
    }
  } catch (...) { failure = std::current_exception(); }
  if (failure) {
    if (!state->finished) (void)state->stopping.cancel();
    while (!state->finished) co_await wait_writer_async(state, {});
    std::rethrow_exception(failure);
  }
}

static cardio::promise<void> close_webdav_writer_async(
    std::shared_ptr<WebdavWriteState> state, cardio::cancellation cancellation) {
  if (state->closed) co_return;
  state->closed = true;
  std::exception_ptr failure;
  try {
    cancellation.throw_if_cancellation_requested();
    state->parent_cancellation.throw_if_cancellation_requested();
    if (state->accepted != state->expected)
      throw std::runtime_error("WebDAV upload source ended before its declared size");
    while (!state->finished) co_await wait_writer_async(state, cancellation);
    cancellation.throw_if_cancellation_requested();
    state->parent_cancellation.throw_if_cancellation_requested();
    if (state->failure) std::rethrow_exception(state->failure);
  } catch (...) { failure = std::current_exception(); }
  if (failure) {
    if (!state->finished) (void)state->stopping.cancel();
    while (!state->finished) co_await wait_writer_async(state, {});
    std::rethrow_exception(failure);
  }
}

class WebdavWriter final : public RemoteFileWriter {
  std::shared_ptr<WebdavWriteState> state;
public:
  explicit WebdavWriter(std::shared_ptr<WebdavWriteState> state) : state(std::move(state)) {}
  ~WebdavWriter() override {
    // A pending write and its request own the state independently of this
    // handle, including cancellation while the bounded buffer is full.
    if (!state->finished) (void)state->stopping.cancel();
  }
  RemoteFileCreationState creation_state() const noexcept override { return state->creation; }
  cardio::promise<void> write_all_async(
      std::span<const std::byte> input, cardio::cancellation cancellation) override {
    return write_webdav_stream_async(state, input, cancellation);
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    return close_webdav_writer_async(state, cancellation);
  }
};

cardio::promise<std::unique_ptr<RemoteFileWriter>> open_webdav_writer_async(
    std::shared_ptr<CurlHttpSession> session, WebdavEndpoint endpoint,
    std::string path, std::uint64_t expected_size, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max()))
    throw std::invalid_argument("WebDAV upload size is not representable by libcurl");
  auto state = std::make_shared<WebdavWriteState>();
  state->session = std::move(session);
  state->url = webdav_resource_url(endpoint, webdav_virtual_path(std::move(path)));
  state->expected = expected_size;
  state->parent_cancellation = cancellation;
  auto writer = std::make_unique<WebdavWriter>(state);
  auto sending = send_webdav_request_async(state);
  cardio::fire_and_forget(std::move(sending));
  if (state->failure) std::rethrow_exception(state->failure);
  co_return writer;
}

} // namespace elder_terms
