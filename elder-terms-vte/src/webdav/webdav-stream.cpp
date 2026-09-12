#include "webdav-stream.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

namespace elder_terms {

struct WebdavReadState {
  std::shared_ptr<CurlHttpSession> session;
  WebdavEndpoint endpoint;
  std::string path;
  cardio::cancellation_source stopping;
  cardio::cancellation parent_cancellation;
  std::array<std::byte, 256 * 1024> bytes{};
  std::size_t head = 0;
  std::size_t count = 0;
  std::size_t paused_chunk = 0;
  bool finished = false;
  bool closed = false;
  std::exception_ptr failure;
  std::shared_ptr<cardio::promise_source<void>> waiting;
};

static void wake_reader(WebdavReadState &state) {
  auto waiting = std::exchange(state.waiting, {});
  if (waiting) waiting->try_resolve();
}

static cardio::promise<void> wait_reader_async(
    std::shared_ptr<WebdavReadState> state, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (state->waiting) throw std::logic_error("Concurrent operations on one WebDAV reader");
  auto source = std::make_shared<cardio::promise_source<void>>();
  auto pending = source->get_promise();
  state->waiting = source;
  auto registration = cancellation.on_cancellation_requested([source] { source->try_cancel(); });
  std::exception_ptr failure;
  try { co_await pending; } catch (...) { failure = std::current_exception(); }
  if (state->waiting == source) state->waiting.reset();
  if (failure) std::rethrow_exception(failure);
}

static std::size_t receive_stream_chunk(WebdavReadState &state,
                                        std::span<const std::byte> bytes) {
  if (bytes.size() > state.bytes.size())
    throw std::runtime_error("WebDAV response chunk exceeds the receive buffer");
  if (bytes.size() > state.bytes.size() - state.count) {
    // A paused callback consumes none of its chunk. libcurl presents exactly
    // these bytes again after the reader makes enough space.
    state.paused_chunk = bytes.size();
    return CURL_WRITEFUNC_PAUSE;
  }
  const auto tail = (state.head + state.count) % state.bytes.size();
  const auto first = std::min(bytes.size(), state.bytes.size() - tail);
  std::copy_n(bytes.begin(), first, state.bytes.begin() + tail);
  std::copy(bytes.begin() + first, bytes.end(), state.bytes.begin());
  state.count += bytes.size();
  wake_reader(state);
  return bytes.size();
}

static cardio::promise<void> receive_webdav_response_async(
    std::shared_ptr<WebdavReadState> state, cardio::cancellation cancellation) {
  try {
    auto combined = cardio::cancellations::any(cancellation, state->stopping.get_cancellation());
    auto url = webdav_resource_url(state->endpoint, state->path);
    for (unsigned redirects = 0;; ++redirects) {
      CurlHttpRequest request;
      request.method = "GET";
      request.url = url;
      request.receive = [state](std::span<const std::byte> bytes) {
        return receive_stream_chunk(*state, bytes);
      };
      auto pending = perform_http_request_async(state->session, std::move(request), combined.get_cancellation());
      const auto result = co_await pending;
      if (result.code != CURLE_OK) throw webdav_http_error(result);
      if (result.status == 200) break;
      if (result.status != 301 && result.status != 302 && result.status != 307 && result.status != 308)
        throw webdav_http_error(result);
      if (redirects == 5 || !result.headers.contains("location"))
        throw std::runtime_error("WebDAV redirect limit or missing location");
      const auto &location = result.headers.at("location");
      const auto path = webdav_reference_path(state->endpoint, url, location);
      url = webdav_resource_url(state->endpoint, path);
      if (location.ends_with('/') && !url.ends_with('/')) url += '/';
    }
  } catch (...) { state->failure = std::current_exception(); }
  state->finished = true;
  wake_reader(*state);
}

static cardio::promise<std::size_t> read_webdav_stream_async(
    std::shared_ptr<WebdavReadState> state, std::span<std::byte> buffer,
    cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (state->closed) throw std::runtime_error("WebDAV reader is closed");
    state->parent_cancellation.throw_if_cancellation_requested();
    if (buffer.empty()) co_return 0;
    while (!state->count && !state->finished) co_await wait_reader_async(state, cancellation);
    cancellation.throw_if_cancellation_requested();
    state->parent_cancellation.throw_if_cancellation_requested();
    if (state->failure) std::rethrow_exception(state->failure);
    const auto count = std::min(buffer.size(), state->count);
    const auto first = std::min(count, state->bytes.size() - state->head);
    std::copy_n(state->bytes.begin() + state->head, first, buffer.begin());
    std::copy_n(state->bytes.begin(), count - first, buffer.begin() + first);
    state->head = (state->head + count) % state->bytes.size();
    state->count -= count;
    if (!state->finished && state->paused_chunk &&
        state->bytes.size() - state->count >= state->paused_chunk) {
      state->paused_chunk = 0;
      resume_curl_http_session(state->session);
    }
    co_return count;
  }

static cardio::promise<void> close_webdav_stream_async(
    std::shared_ptr<WebdavReadState> state, cardio::cancellation cancellation) {
    if (state->closed) co_return;
    state->closed = true;
    if (!state->finished) (void)state->stopping.cancel();
    while (!state->finished) co_await wait_reader_async(state, {});
    cancellation.throw_if_cancellation_requested();
  }

class WebdavReader final : public RemoteFileReader {
  std::shared_ptr<WebdavReadState> state;

public:
  explicit WebdavReader(std::shared_ptr<WebdavReadState> state) : state(std::move(state)) {}
  ~WebdavReader() override {
    // Pending reads and the root request retain state independently of this
    // handle until cancellation has retired every callback and socket watch.
    if (!state->finished) (void)state->stopping.cancel();
  }
  cardio::promise<std::size_t> read_async(
      std::span<std::byte> buffer, cardio::cancellation cancellation) override {
    return read_webdav_stream_async(state, buffer, cancellation);
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    return close_webdav_stream_async(state, cancellation);
  }
};

cardio::promise<std::unique_ptr<RemoteFileReader>> open_webdav_reader_async(
    std::shared_ptr<CurlHttpSession> session, WebdavEndpoint endpoint,
    std::string path, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto state = std::make_shared<WebdavReadState>();
  state->session = std::move(session);
  state->parent_cancellation = cancellation;
  state->endpoint = std::move(endpoint);
  state->path = webdav_virtual_path(std::move(path));
  auto reader = std::make_unique<WebdavReader>(state);
  auto receiving = receive_webdav_response_async(state, cancellation);
  cardio::fire_and_forget(std::move(receiving));
  std::exception_ptr failure;
  try {
    while (!state->count && !state->finished) co_await wait_reader_async(state, cancellation);
    cancellation.throw_if_cancellation_requested();
    if (state->failure) std::rethrow_exception(state->failure);
  } catch (...) { failure = std::current_exception(); }
  if (failure) {
    co_await reader->close_async({});
    std::rethrow_exception(failure);
  }
  co_return reader;
}

} // namespace elder_terms
