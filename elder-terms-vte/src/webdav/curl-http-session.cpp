#include "curl-http-session.h"
#include "../tls/certificate-policy.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace elder_terms {

using HttpClock = std::chrono::steady_clock;

static bool has_only_unsupported_authentication(const CurlHttpResult &result) {
  const auto header = result.headers.find("www-authenticate");
  if (header == result.headers.end()) return false;
  bool found = false;
  bool quoted = false;
  bool escaped = false;
  std::size_t start = 0;
  const auto &value = header->second;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index < value.size()) {
      const auto character = value[index];
      if (escaped) { escaped = false; continue; }
      if (quoted && character == '\\') { escaped = true; continue; }
      if (character == '"') { quoted = !quoted; continue; }
      if (quoted || character != ',') continue;
    }
    auto segment = std::string_view(value).substr(start, index - start);
    start = index + 1;
    while (!segment.empty() && g_ascii_isspace(segment.front())) segment.remove_prefix(1);
    const auto end = segment.find_first_of(" \t=");
    auto token = std::string(segment.substr(0, end));
    auto remainder = end == std::string_view::npos ? std::string_view{} : segment.substr(end);
    while (!remainder.empty() && g_ascii_isspace(remainder.front())) remainder.remove_prefix(1);
    // A comma can separate authentication parameters as well as challenges.
    // Never mistake a quoted realm or a parameter name for an offered scheme.
    if (token.empty() || remainder.starts_with('=')) continue;
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
    if (token == "basic" || token == "digest") return false;
    found = true;
  }
  return found;
}

std::runtime_error webdav_http_error(const CurlHttpResult &result) {
  const char *reason = curl_easy_strerror(result.code);
  if (result.code == CURLE_OK) {
    switch (result.status) {
    case 401:
      reason = has_only_unsupported_authentication(result)
          ? "Unsupported authentication; WebDAV supports Basic and Digest"
          : "Authentication was rejected";
      break;
    case 403: reason = "Access denied"; break;
    case 404: reason = "Resource not found"; break;
    case 405: case 501: reason = "The server does not support this WebDAV operation"; break;
    case 409: reason = "Parent collection or resource conflict"; break;
    case 412: reason = "Destination exists or a request precondition failed"; break;
    case 423: reason = "Resource is locked"; break;
    case 507: reason = "Insufficient server storage"; break;
    default: reason = "Unexpected server response"; break;
    }
  }
  return std::runtime_error("WebDAV request failed (HTTP " + std::to_string(result.status) +
      ", curl " + std::to_string(result.code) + "): " + reason);
}


static void require_curl(CURLcode code) {
  if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
}

static void require_multi(CURLMcode code) {
  if (code != CURLM_OK) throw std::runtime_error(curl_multi_strerror(code));
}

struct HttpCurlGlobal {
  HttpCurlGlobal() { require_curl(curl_global_init(CURL_GLOBAL_DEFAULT)); }
  ~HttpCurlGlobal() { curl_global_cleanup(); }
};

struct HttpWake {
  curl_socket_t socket = CURL_SOCKET_TIMEOUT;
  int events = 0;
};

struct CurlHttpSession {
  WebdavConnectionSettings settings;
  std::string password;
  WebdavCertificateConfirmation confirm_certificate;
  std::shared_ptr<TlsCertificatePolicy> certificates;
  CURLM *multi = nullptr;
  CURL *easy = nullptr;
  cardio::primitives::mutex operations;
  cardio::cancellation_source stopping;
  std::map<curl_socket_t, int> sockets;
  std::optional<HttpClock::time_point> deadline;
  std::shared_ptr<cardio::promise_source<HttpWake>> waiting;
  std::exception_ptr callback_failure;
  bool resume_pending = false;

  ~CurlHttpSession() {
    // perform_http_request_async retains the session until all watchers have
    // settled. Cleanup callbacks may still update sockets, so the state outlives curl.
    if (easy) curl_easy_cleanup(easy);
    if (multi) curl_multi_cleanup(multi);
  }
};

static int socket_changed(CURL *, curl_socket_t socket, int interest,
                          void *data, void *) noexcept {
  auto &session = *static_cast<CurlHttpSession *>(data);
  try {
    if (interest == CURL_POLL_REMOVE || interest == CURL_POLL_NONE)
      session.sockets.erase(socket);
    else
      session.sockets[socket] = interest;
    return 0;
  } catch (...) {
    session.callback_failure = std::current_exception();
    return -1;
  }
}

static int timer_changed(CURLM *, long milliseconds, void *data) noexcept {
  auto &session = *static_cast<CurlHttpSession *>(data);
  session.deadline = milliseconds < 0 ? std::nullopt
      : std::optional(HttpClock::now() + std::chrono::milliseconds(milliseconds));
  return 0;
}

static cardio::promise<void> observe_socket_async(
    std::shared_ptr<cardio::promise_source<HttpWake>> winner,
    curl_socket_t socket, int interest, cardio::cancellation cancellation) {
  try {
    auto events = cardio::fd_event::none;
    if (interest & CURL_POLL_IN) events |= cardio::fd_event::read;
    if (interest & CURL_POLL_OUT) events |= cardio::fd_event::write;
    const auto ready = co_await cardio::from_fd(socket, events, cancellation);
    int selected = 0;
    if ((ready & cardio::fd_event::read) != cardio::fd_event::none) selected |= CURL_CSELECT_IN;
    if ((ready & cardio::fd_event::write) != cardio::fd_event::none) selected |= CURL_CSELECT_OUT;
    if ((ready & (cardio::fd_event::error | cardio::fd_event::hangup)) != cardio::fd_event::none)
      selected |= CURL_CSELECT_ERR;
    winner->try_resolve(HttpWake{socket, selected});
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
    winner->try_reject(std::current_exception());
  }
}

static cardio::promise<void> observe_timer_async(
    std::shared_ptr<cardio::promise_source<HttpWake>> winner,
    HttpClock::time_point deadline, cardio::cancellation cancellation) {
  try {
    const auto remaining = deadline - HttpClock::now();
    const auto milliseconds = std::max<std::int64_t>(0,
        std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
    co_await cardio::promises::delay(static_cast<std::uint64_t>(milliseconds), cancellation);
    winner->try_resolve(HttpWake{});
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
    winner->try_reject(std::current_exception());
  }
}

static cardio::promise<HttpWake> wait_http_event_async(
    const std::shared_ptr<CurlHttpSession> &session,
    std::optional<HttpClock::time_point> idle_deadline,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (session->resume_pending) co_return HttpWake{};
  auto winner = std::make_shared<cardio::promise_source<HttpWake>>();
  auto completed = winner->get_promise();
  session->waiting = winner;
  cardio::cancellation_source watchers;
  auto registration = cancellation.on_cancellation_requested([winner] { winner->try_cancel(); });
  std::vector<cardio::promise<void>> tasks;
  std::exception_ptr failure;
  HttpWake event;
  try {
    for (const auto &[socket, interest] : session->sockets)
      tasks.push_back(observe_socket_async(winner, socket, interest, watchers.get_cancellation()));
    auto deadline = session->deadline;
    if (idle_deadline && (!deadline || *idle_deadline < *deadline)) deadline = idle_deadline;
    if (deadline) tasks.push_back(observe_timer_async(winner, *deadline, watchers.get_cancellation()));
    event = co_await completed;
  } catch (...) {
    failure = std::current_exception();
  }
  session->waiting.reset();
  (void)watchers.cancel();
  // Retire every old FD watch before curl can close/reuse a descriptor or alter
  // its interests. A readiness event never survives into the next curl action.
  for (auto &task : tasks) co_await task;
  if (failure) std::rethrow_exception(failure);
  co_return event;
}

struct HttpOperation {
  CurlHttpRequest request;
  CurlHttpResult result;
  std::array<char, CURL_ERROR_SIZE> error{};
  std::exception_ptr failure;
  HttpClock::time_point activity = HttpClock::now();
  curl_off_t downloaded = 0;
  curl_off_t uploaded = 0;
  std::size_t header_bytes = 0;
  std::uint64_t input_consumed = 0;
  int pause_mask = CURLPAUSE_CONT;
};

static std::size_t receive_body(char *bytes, std::size_t size,
                                std::size_t count, void *data) noexcept {
  auto &operation = *static_cast<HttpOperation *>(data);
  try {
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
      throw std::runtime_error("WebDAV response size overflow");
    const auto length = size * count;
    operation.activity = HttpClock::now();
    if (operation.request.receive && operation.result.status >= 200 && operation.result.status < 300) {
      const auto received = operation.request.receive(
          {reinterpret_cast<const std::byte *>(bytes), length});
      if (received == CURL_WRITEFUNC_PAUSE) operation.pause_mask |= CURLPAUSE_RECV;
      else operation.pause_mask &= ~CURLPAUSE_RECV;
      return received;
    }
    constexpr std::size_t limit = 32 * 1024 * 1024;
    if (length > limit - operation.result.body.size())
      throw std::runtime_error("WebDAV response exceeds the 32 MiB limit");
    operation.result.body.append(bytes, length);
    return length;
  } catch (...) {
    operation.failure = std::current_exception();
    return 0;
  }
}

static std::size_t send_body(char *bytes, std::size_t size,
                             std::size_t count, void *data) noexcept {
  auto &operation = *static_cast<HttpOperation *>(data);
  try {
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
      throw std::runtime_error("WebDAV upload buffer size overflow");
    const auto capacity = size * count;
    const auto sent = operation.request.send({reinterpret_cast<std::byte *>(bytes), capacity});
    if (sent == CURL_READFUNC_PAUSE) {
      operation.pause_mask |= CURLPAUSE_SEND;
      return sent;
    }
    operation.pause_mask &= ~CURLPAUSE_SEND;
    if (sent > capacity || sent > *operation.request.upload_size - operation.input_consumed)
      throw std::runtime_error("WebDAV upload exceeded its declared size");
    operation.input_consumed += sent;
    if (sent) operation.activity = HttpClock::now();
    return sent;
  } catch (...) {
    operation.failure = std::current_exception();
    return CURL_READFUNC_ABORT;
  }
}

static int seek_body(void *data, curl_off_t offset, int origin) noexcept {
  auto &operation = *static_cast<HttpOperation *>(data);
  if (offset == 0 && origin == SEEK_SET && operation.input_consumed == 0)
    return CURL_SEEKFUNC_OK;
  try {
    throw std::runtime_error("WebDAV cannot replay an upload after its input has been consumed");
  } catch (...) { operation.failure = std::current_exception(); }
  return CURL_SEEKFUNC_FAIL;
}

static std::size_t receive_header(char *bytes, std::size_t size,
                                  std::size_t count, void *data) noexcept {
  auto &operation = *static_cast<HttpOperation *>(data);
  try {
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
      throw std::runtime_error("WebDAV header size overflow");
    const auto length = size * count;
    if (length > 1024 * 1024 - operation.header_bytes)
      throw std::runtime_error("WebDAV headers exceed the 1 MiB limit");
    operation.header_bytes += length;
    operation.activity = HttpClock::now();
    std::string_view line(bytes, length);
    if (line.starts_with("HTTP/")) {
      operation.result.headers.clear();
      operation.result.body.clear();
      const auto space = line.find(' ');
      if (space == std::string_view::npos) throw std::runtime_error("Invalid HTTP status line");
      const auto parsed = std::from_chars(line.data() + space + 1, line.data() + line.size(), operation.result.status);
      if (parsed.ec != std::errc{}) throw std::runtime_error("Invalid HTTP response status");
      // Abort before libcurl can replay even a small internally retained body.
      // A challenge before consuming input can still negotiate Basic/Digest.
      if (operation.result.status == 401 && operation.request.send && operation.input_consumed)
        throw std::runtime_error("WebDAV authentication changed after upload input was consumed; the request was not replayed");
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
      std::string name(line.substr(0, colon));
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) { return g_ascii_tolower(value); });
      auto value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
      if (name == "content-encoding" && operation.result.status >= 200 &&
          operation.result.status < 300 && !value.empty() &&
          g_ascii_strcasecmp(std::string(value).c_str(), "identity") != 0)
        throw std::runtime_error("WebDAV server returned an unsupported Content-Encoding; use an endpoint that honors Accept-Encoding: identity");
      auto &stored = operation.result.headers[name];
      if (!stored.empty()) stored += ", ";
      stored.append(value);
    }
    return length;
  } catch (...) {
    operation.failure = std::current_exception();
    return 0;
  }
}

static int progress_changed(void *data, curl_off_t, curl_off_t downloaded,
                             curl_off_t, curl_off_t uploaded) noexcept {
  auto &operation = *static_cast<HttpOperation *>(data);
  if (downloaded != operation.downloaded || uploaded != operation.uploaded) {
    operation.activity = HttpClock::now();
    operation.downloaded = downloaded;
    operation.uploaded = uploaded;
  }
  return 0;
}

std::shared_ptr<CurlHttpSession> create_curl_http_session(
    WebdavConnectionSettings settings, std::string password,
    WebdavCertificateConfirmation confirm_certificate) {
  if (!settings.validation_errors.empty()) throw std::invalid_argument(settings.validation_errors.front());
  if (settings.scheme != "https" && settings.scheme != "http") throw std::invalid_argument("WebDAV requires HTTP or HTTPS");
  static const HttpCurlGlobal global;
  (void)global;
  const auto *version = curl_version_info(CURLVERSION_NOW);
  if (!version || version->version_num < 0x075801) throw std::runtime_error("WebDAV requires libcurl 7.88.1 or newer");
  if (!(version->features & CURL_VERSION_ASYNCHDNS)) throw std::runtime_error("WebDAV requires libcurl with asynchronous DNS");
  bool supported = false;
  for (auto protocol = version->protocols; protocol && *protocol; ++protocol)
    supported = supported || settings.scheme == *protocol;
  if (!supported) throw std::runtime_error("libcurl does not support the selected WebDAV scheme");
  auto session = std::make_shared<CurlHttpSession>();
  session->settings = std::move(settings);
  session->password = std::move(password);
  session->confirm_certificate = std::move(confirm_certificate);
  if (session->settings.prompt_certificate && session->settings.scheme == "https")
    session->certificates = create_tls_certificate_policy(
        session->settings.address, session->settings.port, session->settings.scheme, 1);
  session->multi = curl_multi_init();
  session->easy = curl_easy_init();
  if (!session->multi || !session->easy) throw std::runtime_error("Could not initialize WebDAV HTTP transport");
  require_multi(curl_multi_setopt(session->multi, CURLMOPT_SOCKETFUNCTION, socket_changed));
  require_multi(curl_multi_setopt(session->multi, CURLMOPT_SOCKETDATA, session.get()));
  require_multi(curl_multi_setopt(session->multi, CURLMOPT_TIMERFUNCTION, timer_changed));
  require_multi(curl_multi_setopt(session->multi, CURLMOPT_TIMERDATA, session.get()));
  return session;
}

static void clear_http_borrowed_options(CURL *easy) noexcept {
  (void)curl_easy_setopt(easy, CURLOPT_POSTFIELDS, static_cast<char *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, static_cast<curl_write_callback>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, static_cast<void *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, static_cast<curl_write_callback>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_HEADERDATA, static_cast<void *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_READFUNCTION, static_cast<curl_read_callback>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_READDATA, static_cast<void *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_SEEKFUNCTION, static_cast<curl_seek_callback>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_SEEKDATA, static_cast<void *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, static_cast<curl_xferinfo_callback>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_XFERINFODATA, static_cast<void *>(nullptr));
  (void)curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
  (void)curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, static_cast<char *>(nullptr));
}

static cardio::promise<CurlHttpResult> perform_http_attempt_async(
    std::shared_ptr<CurlHttpSession> session, CurlHttpRequest request,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  HttpOperation operation;
  operation.request = std::move(request);
  auto *easy = session->easy;

  session->callback_failure = {};
  session->resume_pending = false;
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
  // Keep transfer bytes identical to the stored file content without an
  // unbounded decompression buffer when a slow consumer pauses the stream.
  operation.request.headers.emplace_back("Accept-Encoding: identity");
  for (const auto &header : operation.request.headers) {
    if (header.find_first_of("\r\n") != std::string::npos || header.find('\0') != std::string::npos)
      throw std::invalid_argument("Invalid WebDAV request header");
    auto *next = curl_slist_append(headers.get(), header.c_str());
    if (!next) throw std::bad_alloc();
    (void)headers.release();
    headers.reset(next);
  }
  // All exits, including failed option setup, release borrowed request data
  // before the operation and header list are destroyed.
  // curl_easy_reset also discards negotiated Digest authentication. Retain that
  // session state while releasing every pointer borrowed from this operation.
  const auto reset_options = std::unique_ptr<CURL, decltype(&clear_http_borrowed_options)>(easy, clear_http_borrowed_options);
  require_curl(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, static_cast<char *>(nullptr)));
  require_curl(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t{0}));
  require_curl(curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L));
  const auto &method = operation.request.method;
  const bool mutation = method != "GET" && method != "HEAD" && method != "PROPFIND" && method != "OPTIONS";
  // Fresh HTTP/1.1 mutation connections avoid stale-connection and HTTP/2
  // stream retries. Redirects remain disabled, and consumed input cannot seek.
  require_curl(curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, mutation || session->certificates ? 1L : 0L));
  require_curl(curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, mutation || session->certificates ? 1L : 0L));
  require_curl(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
      static_cast<long>(mutation ? CURL_HTTP_VERSION_1_1 : CURL_HTTP_VERSION_NONE)));
  require_curl(curl_easy_setopt(easy, CURLOPT_URL, operation.request.url.c_str()));
  require_curl(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, session->settings.scheme.c_str()));
  require_curl(curl_easy_setopt(easy, CURLOPT_PROXY, ""));
  require_curl(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
  require_curl(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L));
  require_curl(curl_easy_setopt(easy, CURLOPT_PATH_AS_IS, 1L));
  require_curl(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L));
  // The shared OpenSSL policy performs hostname validation before accepting
  // any HTTP bytes. Its exceptions belong only to this immutable endpoint.
  require_curl(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, session->certificates ? 0L : 2L));
  if (session->certificates) {
    // Observe the actual leaf on every request: neither an old HTTP connection
    // nor TLS session resumption may bypass a changed-certificate decision.
    require_curl(curl_easy_setopt(easy, CURLOPT_SSL_SESSIONID_CACHE, 0L));
    require_curl(curl_easy_setopt(easy, CURLOPT_SSL_CTX_FUNCTION, configure_tls_certificate_context));
    require_curl(curl_easy_setopt(easy, CURLOPT_SSL_CTX_DATA, session->certificates.get()));
  }
  require_curl(curl_easy_setopt(easy, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2)));
  if (!session->settings.ca_file.empty())
    require_curl(curl_easy_setopt(easy, CURLOPT_CAINFO, session->settings.ca_file.c_str()));
  require_curl(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, static_cast<long>(session->settings.connect_timeout_seconds)));
  if (session->settings.authentication != WebdavAuthentication::none) {
    const auto auth = session->settings.authentication;
    const long allowed = auth == WebdavAuthentication::basic ? CURLAUTH_BASIC
        : auth == WebdavAuthentication::digest ? CURLAUTH_DIGEST : CURLAUTH_BASIC | CURLAUTH_DIGEST;
    require_curl(curl_easy_setopt(easy, CURLOPT_HTTPAUTH, allowed));
    require_curl(curl_easy_setopt(easy, CURLOPT_USERNAME, session->settings.username.c_str()));
    require_curl(curl_easy_setopt(easy, CURLOPT_PASSWORD, session->password.c_str()));
  }
  if (operation.request.send) {
    if (!operation.request.upload_size || !operation.request.body.empty() ||
        *operation.request.upload_size > static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max()))
      throw std::invalid_argument("WebDAV upload requires one representable known size");
    require_curl(curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L));
    require_curl(curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(*operation.request.upload_size)));
    require_curl(curl_easy_setopt(easy, CURLOPT_READFUNCTION, send_body));
    require_curl(curl_easy_setopt(easy, CURLOPT_READDATA, &operation));
    require_curl(curl_easy_setopt(easy, CURLOPT_SEEKFUNCTION, seek_body));
    require_curl(curl_easy_setopt(easy, CURLOPT_SEEKDATA, &operation));
  } else if (operation.request.upload_size) {
    throw std::invalid_argument("WebDAV upload has no body producer");
  }
  if (!operation.request.body.empty()) {
    require_curl(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(operation.request.body.size())));
    require_curl(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, operation.request.body.data()));
  }
  require_curl(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, operation.request.method.c_str()));
  require_curl(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get()));
  require_curl(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive_body));
  require_curl(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &operation));
  require_curl(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, receive_header));
  require_curl(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &operation));
  require_curl(curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress_changed));
  require_curl(curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &operation));
  require_curl(curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L));
  require_curl(curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, operation.error.data()));
  require_multi(curl_multi_add_handle(session->multi, easy));
  std::exception_ptr failure;
  try {
    auto event = HttpWake{};
    bool complete = false;
    bool pause_initialized = false;
    while (!complete) {
      cancellation.throw_if_cancellation_requested();
      if (session->resume_pending) {
        session->resume_pending = false;
        operation.pause_mask = CURLPAUSE_CONT;
        operation.activity = HttpClock::now();
        require_curl(curl_easy_pause(easy, CURLPAUSE_CONT));
        // Resumption can synchronously change curl socket interests. Rebuild
        // readiness watches instead of forwarding an event from before resume.
        event = HttpWake{};
      }
      int running = 0;
      require_multi(curl_multi_socket_action(session->multi, event.socket, event.events, &running));
      if (session->callback_failure) std::rethrow_exception(session->callback_failure);
      if (operation.failure) std::rethrow_exception(operation.failure);
      int remaining = 0;
      while (auto *message = curl_multi_info_read(session->multi, &remaining)) {
        if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
          operation.result.code = message->data.result;
          complete = true;
        }
      }
      if (complete) break;
      if ((operation.pause_mask & CURLPAUSE_SEND) &&
          operation.result.status >= 300 && operation.result.status != 401 &&
          session->sockets.empty()) {
        // Older curl can retain the send-pause bit after reading a refusal,
        // with no remaining socket interests. Release that completed upload
        // direction so its final HTTP response can finish without more input.
        operation.pause_mask &= ~CURLPAUSE_SEND;
        require_curl(curl_easy_pause(easy, operation.pause_mask));
      }
      if (!pause_initialized) {
        // An easy handle can retain pause state after cancellation or an early
        // server response. Once curl attaches a connection, restore only the
        // pauses requested by this operation's callbacks. Before attachment,
        // curl_easy_pause returns CURLE_BAD_FUNCTION_ARGUMENT; retry on the
        // next normal curl event without polling or resetting Digest state.
        const auto code = curl_easy_pause(easy, operation.pause_mask);
        if (code != CURLE_BAD_FUNCTION_ARGUMENT) {
          require_curl(code);
          pause_initialized = true;
        }
      }
      const auto idle_deadline = operation.pause_mask != CURLPAUSE_CONT ? std::nullopt
          : std::optional(operation.activity + std::chrono::seconds(session->settings.idle_timeout_seconds));
      if (idle_deadline && HttpClock::now() >= *idle_deadline)
        throw std::runtime_error("WebDAV connection stopped making progress");
      event = co_await wait_http_event_async(session, idle_deadline, cancellation);
    }
    require_curl(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &operation.result.status));
    operation.result.error = operation.error.data();
  } catch (...) {
    failure = std::current_exception();
  }
  const auto removed = curl_multi_remove_handle(session->multi, easy);
  if (failure) std::rethrow_exception(failure);
  require_multi(removed);
  co_return std::move(operation.result);
}

cardio::promise<CurlHttpResult> perform_http_request_async(
    std::shared_ptr<CurlHttpSession> session, CurlHttpRequest request,
    cardio::cancellation cancellation) {
  auto combined = cardio::cancellations::any(cancellation, session->stopping.get_cancellation());
  cancellation = combined.get_cancellation();
  auto lock = std::move(co_await session->operations.lock(cancellation));
  const bool read_only = request.method == "GET" || request.method == "HEAD" ||
      request.method == "PROPFIND" || request.method == "OPTIONS";
  for (unsigned confirmations = 0;; ++confirmations) {
    cancellation.throw_if_cancellation_requested();
    if (session->certificates) {
      session->certificates->failure.reset();
      session->certificates->callback_failure = {};
    }
    auto result = co_await perform_http_attempt_async(session, request, cancellation);
    if (!session->certificates) co_return result;
    if (session->certificates->callback_failure)
      std::rethrow_exception(session->certificates->callback_failure);
    if (result.code != CURLE_PEER_FAILED_VERIFICATION ||
        !session->certificates->failure || !session->confirm_certificate)
      co_return result;
    if (confirmations == 16)
      throw std::runtime_error("WebDAV certificate verification did not converge");
    // The failed attempt has already detached its easy handle and retired all
    // watchers. Confirmation runs outside OpenSSL and retains only copied data.
    const auto failure = *session->certificates->failure;
    const bool accepted = co_await session->confirm_certificate(failure, cancellation);
    cancellation.throw_if_cancellation_requested();
    if (!accepted) {
      (void)session->stopping.cancel();
      cancellation.throw_if_cancellation_requested();
    }
    approve_tls_certificate_failure(*session->certificates, failure);
    if (read_only && result.status == 0 && result.code == CURLE_PEER_FAILED_VERIFICATION)
      continue;
    // A mutation remains failed even when its handshake failed before HTTP.
    // Approval applies to a distinct explicit retry, never an automatic replay.
    throw std::runtime_error("WebDAV certificate accepted for this connection; the failed operation was not retried");
  }
}

void resume_curl_http_session(const std::shared_ptr<CurlHttpSession> &session) {
  session->resume_pending = true;
  if (session->waiting) session->waiting->try_resolve(HttpWake{});
}

cardio::promise<void> stop_curl_http_session_async(std::shared_ptr<CurlHttpSession> session) {
  (void)session->stopping.cancel();
  auto lock = std::move(co_await session->operations.lock());
}

} // namespace elder_terms
