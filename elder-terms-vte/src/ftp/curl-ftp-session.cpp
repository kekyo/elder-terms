#include "curl-ftp-session.h"
#include "ftps-certificate.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace elder_terms {

struct CurlGlobal {
  CurlGlobal() {
    const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
      throw std::runtime_error(curl_easy_strerror(code));
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

static void require_curl(CURLcode code) {
  if (code != CURLE_OK) {
    throw std::runtime_error(curl_easy_strerror(code));
  }
}

struct CurlOperation {
  CurlFtpRequest request;
  std::optional<FtpCertificateFailure> certificate_approval;
  cardio::cancellation cancellation;
  cardio::promise_source<CurlFtpResult> completion;
  std::exception_ptr callback_failure;
  std::array<char, CURL_ERROR_SIZE> error{};
  curl_slist *quote = nullptr;
  curl_slist *prequote = nullptr;

  ~CurlOperation() {
    curl_slist_free_all(quote);
    curl_slist_free_all(prequote);
  }
};

static curl_slist *make_commands(const std::vector<std::string> &commands) {
  curl_slist *result = nullptr;
  for (const auto &command : commands) {
    curl_slist *next = curl_slist_append(result, command.c_str());
    if (next == nullptr) {
      curl_slist_free_all(result);
      throw std::bad_alloc();
    }
    result = next;
  }
  return result;
}


static long curl_tls_version(FtpTlsVersion version, bool maximum) {
  switch (version) {
  case FtpTlsVersion::tls10: return maximum ? static_cast<long>(CURL_SSLVERSION_MAX_TLSv1_0) : static_cast<long>(CURL_SSLVERSION_TLSv1_0);
  case FtpTlsVersion::tls11: return maximum ? static_cast<long>(CURL_SSLVERSION_MAX_TLSv1_1) : static_cast<long>(CURL_SSLVERSION_TLSv1_1);
  case FtpTlsVersion::tls12: return maximum ? static_cast<long>(CURL_SSLVERSION_MAX_TLSv1_2) : static_cast<long>(CURL_SSLVERSION_TLSv1_2);
  case FtpTlsVersion::tls13: return maximum ? static_cast<long>(CURL_SSLVERSION_MAX_TLSv1_3) : static_cast<long>(CURL_SSLVERSION_TLSv1_3);
  }
  throw std::invalid_argument("Invalid FTPS TLS version");
}

// Network calls and callbacks belong to this worker. The caller only posts
// requests and wakes the condition variable; it never accesses an easy handle.
struct CurlWorker {
  FtpClientOpenOptions options;
  std::shared_ptr<FtpCertificatePolicy> certificates;
  std::mutex mutex;
  std::condition_variable changed;
  std::uint64_t revision = 0;
  std::deque<std::shared_ptr<CurlOperation>> pending;
  std::atomic_bool stopping = false;
  std::mutex socket_mutex;
  std::set<curl_socket_t> open_sockets;
  CURL *easy = nullptr;
  std::shared_ptr<CurlOperation> operation;
  cardio::promise_source<void> ready;

  explicit CurlWorker(FtpClientOpenOptions options) : options(std::move(options)) {
    if (this->options.connection.tls_mode != FtpTlsMode::none &&
        this->options.connection.certificate_error_action == FtpCertificateErrorAction::prompt)
      certificates = create_ftp_certificate_policy(this->options.connection);
  }

  void wake() {
    {
      const auto lock = std::lock_guard(mutex);
      ++revision;
    }
    changed.notify_all();
  }

  void request_stop() {
    stopping.store(true);
    wake();
  }

  void shutdown_sockets() {
    const auto lock = std::lock_guard(socket_mutex);
    for (const auto fd : open_sockets) (void)::shutdown(fd, SHUT_RDWR);
  }

  void cancel(const std::shared_ptr<CurlOperation> &op) {
    wake();
    const auto lock = std::lock_guard(socket_mutex);
    // A cancellation callback can already be queued when its registration is
    // released. Never let a late callback interrupt a subsequent operation.
    if (operation == op) {
      for (const auto fd : open_sockets) (void)::shutdown(fd, SHUT_RDWR);
    }
  }

  static int close_socket(void *data, curl_socket_t fd) noexcept {
    auto *worker = static_cast<CurlWorker *>(data);
    const auto lock = std::lock_guard(worker->socket_mutex);
    worker->open_sockets.erase(fd);
    return ::close(fd) == 0 ? 0 : 1;
  }

  static int configure_socket(void *data, curl_socket_t fd,
                                curlsocktype purpose) noexcept {
    auto *worker = static_cast<CurlWorker *>(data);
    try {
      if (check_accepted_socket(data, fd, purpose) != CURL_SOCKOPT_OK) {
        return CURL_SOCKOPT_ERROR;
      }
      const auto lock = std::lock_guard(worker->socket_mutex);
      if (worker->stopping.load() || (worker->operation &&
          worker->operation->cancellation.is_cancellation_requested())) {
        return CURL_SOCKOPT_ERROR;
      }
      worker->open_sockets.insert(fd);
      return CURL_SOCKOPT_OK;
    } catch (...) {
      if (worker->operation) worker->operation->callback_failure = std::current_exception();
      return CURL_SOCKOPT_ERROR;
    }
  }

  void submit(std::shared_ptr<CurlOperation> op) {
    {
      const auto lock = std::lock_guard(mutex);
      if (stopping.load()) {
        op->completion.try_cancel();
        return;
      }
      pending.push_back(std::move(op));
    }
    changed.notify_all();
  }

  std::size_t exchange_bytes(char *bytes, std::size_t length, bool upload) {
    const auto op = operation;
    if (!op) return upload ? CURL_READFUNC_ABORT : CURL_WRITEFUNC_ERROR;
    const auto pause = upload ? CURL_READFUNC_PAUSE : CURL_WRITEFUNC_PAUSE;
    for (;;) {
      std::uint64_t observed;
      {
        const auto lock = std::lock_guard(mutex);
        observed = revision;
      }
      if (stopping.load() || op->cancellation.is_cancellation_requested()) {
        return upload ? CURL_READFUNC_ABORT : CURL_WRITEFUNC_ERROR;
      }
      std::size_t count;
      if (upload) {
        count = op->request.send
            ? op->request.send({reinterpret_cast<std::byte *>(bytes), length}) : 0;
      } else {
        count = op->request.receive
            ? op->request.receive({reinterpret_cast<const std::byte *>(bytes), length}) : length;
      }
      if (count != static_cast<std::size_t>(pause)) return count;
      // Retain the callback's buffer only while this callback is active.
      // Reading the revision before calling the consumer prevents lost wakes.
      auto lock = std::unique_lock(mutex);
      changed.wait(lock, [&] {
        return revision != observed || stopping.load() ||
            op->cancellation.is_cancellation_requested();
      });
    }
  }

  static std::size_t receive(char *bytes, std::size_t size, std::size_t count,
                              void *data) noexcept {
    auto *worker = static_cast<CurlWorker *>(data);
    try {
      return worker->exchange_bytes(bytes, size * count, false);
    } catch (...) {
      if (worker->operation) worker->operation->callback_failure = std::current_exception();
      return CURL_WRITEFUNC_ERROR;
    }
  }

  static std::size_t send(char *bytes, std::size_t size, std::size_t count,
                           void *data) noexcept {
    auto *worker = static_cast<CurlWorker *>(data);
    try {
      return worker->exchange_bytes(bytes, size * count, true);
    } catch (...) {
      if (worker->operation) worker->operation->callback_failure = std::current_exception();
      return CURL_READFUNC_ABORT;
    }
  }

  static std::size_t header(char *bytes, std::size_t size, std::size_t count,
                             void *data) noexcept {
    auto *worker = static_cast<CurlWorker *>(data);
    const auto op = worker->operation;
    try {
      if (worker->certificates && size * count >= 4) {
        const std::string_view line(bytes, size * count);
        if (line.starts_with("150 ") || line.starts_with("125 ")) worker->certificates->channel = FtpTlsChannel::data;
      }
      if (op && op->request.header) op->request.header({bytes, size * count});
      return size * count;
    } catch (...) {
      op->callback_failure = std::current_exception();
      return CURL_WRITEFUNC_ERROR;
    }
  }

  static int progress(void *data, curl_off_t, curl_off_t,
                        curl_off_t, curl_off_t) noexcept {
    auto *worker = static_cast<CurlWorker *>(data);
    return worker->stopping.load() ||
        (worker->operation && worker->operation->cancellation.is_cancellation_requested());
  }

  static int check_accepted_socket(void *data, curl_socket_t fd,
                                    curlsocktype purpose) noexcept {
    if (purpose != CURLSOCKTYPE_ACCEPT) return CURL_SOCKOPT_OK;
    auto *worker = static_cast<CurlWorker *>(data);
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    char *primary = nullptr;
    if (::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &length) != 0 ||
        curl_easy_getinfo(worker->easy, CURLINFO_PRIMARY_IP, &primary) != CURLE_OK ||
        primary == nullptr) return CURL_SOCKOPT_ERROR;
    std::array<unsigned char, 16> expected{};
    if (::inet_pton(peer.ss_family, primary, expected.data()) != 1)
      return CURL_SOCKOPT_ERROR;
    const void *actual = peer.ss_family == AF_INET
        ? static_cast<const void *>(&reinterpret_cast<const sockaddr_in *>(&peer)->sin_addr)
        : static_cast<const void *>(&reinterpret_cast<const sockaddr_in6 *>(&peer)->sin6_addr);
    const std::size_t bytes = peer.ss_family == AF_INET ? 4 : 16;
    return std::memcmp(expected.data(), actual, bytes) == 0
        ? CURL_SOCKOPT_OK : CURL_SOCKOPT_ERROR;
  }


  void perform(const std::shared_ptr<CurlOperation> &op) noexcept {
    {
      const auto lock = std::lock_guard(socket_mutex);
      operation = op;
    }
    std::exception_ptr failure;
    CurlFtpResult result;
    try {
      op->cancellation.throw_if_cancellation_requested();
      if (easy == nullptr) {
        easy = curl_easy_init();
        if (!easy) throw std::bad_alloc();
      }
      if (certificates) {
        certificates->failure.reset();
        certificates->callback_failure = nullptr;
        certificates->channel = FtpTlsChannel::control;
        if (op->certificate_approval) approve_ftp_certificate_failure(*certificates, *op->certificate_approval);
      }
      curl_easy_reset(easy);
      require_curl(curl_easy_setopt(easy, CURLOPT_URL, op->request.url.c_str()));
      require_curl(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, options.connection.tls_mode == FtpTlsMode::implicit_tls ? "ftps" : "ftp"));
      require_curl(curl_easy_setopt(easy, CURLOPT_PROXY, ""));
      require_curl(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
      require_curl(curl_easy_setopt(easy, CURLOPT_USE_SSL, static_cast<long>(options.connection.tls_mode == FtpTlsMode::none ? CURLUSESSL_NONE : CURLUSESSL_ALL)));
      if (options.connection.tls_mode != FtpTlsMode::none) {
        require_curl(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L));
        require_curl(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, certificates ? 0L : 2L));
        require_curl(curl_easy_setopt(easy, CURLOPT_SSLVERSION, (curl_tls_version(options.connection.tls_min_version, false) |
            (options.connection.tls_max_version ? curl_tls_version(*options.connection.tls_max_version, true) : static_cast<long>(CURL_SSLVERSION_MAX_DEFAULT)))));
        require_curl(curl_easy_setopt(easy, CURLOPT_FTPSSLAUTH, static_cast<long>(options.connection.tls_auth_order == FtpTlsAuthOrder::ssl ? CURLFTPAUTH_SSL :
            options.connection.tls_auth_order == FtpTlsAuthOrder::automatic ? CURLFTPAUTH_DEFAULT : CURLFTPAUTH_TLS)));
        if (certificates) {
          // The callback replaces hostname checking and retains peer checking.
          // This worker and its connection cache never change endpoint/policy.
          require_curl(curl_easy_setopt(easy, CURLOPT_SSL_CTX_FUNCTION, configure_ftp_certificate_context));
          require_curl(curl_easy_setopt(easy, CURLOPT_SSL_CTX_DATA, certificates.get()));
        }
        auto ciphers = options.connection.tls_cipher_list;
        if (options.connection.tls_compatibility == FtpTlsCompatibility::openssl_legacy && ciphers.empty()) ciphers = "DEFAULT";
        if (!ciphers.empty()) {
          ciphers += ":!aNULL:!eNULL";
          if (options.connection.tls_compatibility == FtpTlsCompatibility::openssl_legacy) ciphers += ":@SECLEVEL=0";
          require_curl(curl_easy_setopt(easy, CURLOPT_SSL_CIPHER_LIST, ciphers.c_str()));
        }
        if (!options.connection.tls13_cipher_list.empty())
          require_curl(curl_easy_setopt(easy, CURLOPT_TLS13_CIPHERS, options.connection.tls13_cipher_list.c_str()));
        if (!options.connection.ca_file.empty())
          require_curl(curl_easy_setopt(easy, CURLOPT_CAINFO, options.connection.ca_file.c_str()));
      }
      require_curl(curl_easy_setopt(easy, CURLOPT_USERNAME, options.connection.username.c_str()));
      require_curl(curl_easy_setopt(easy, CURLOPT_PASSWORD, options.password.c_str()));
      // RFC 2577 describes PASV address substitution attacks. The control peer
      // is authoritative; only the advertised port is used.
      require_curl(curl_easy_setopt(easy, CURLOPT_FTP_SKIP_PASV_IP, 1L));
      require_curl(curl_easy_setopt(easy, CURLOPT_FTP_USE_EPSV, 1L));
      require_curl(curl_easy_setopt(easy, CURLOPT_FTP_USE_EPRT, 1L));
      require_curl(curl_easy_setopt(easy, CURLOPT_FTPPORT,
          options.connection.data_connection_mode == FtpDataConnectionMode::active ? "-" : nullptr));
      require_curl(curl_easy_setopt(easy, CURLOPT_FTP_FILEMETHOD, static_cast<long>(CURLFTPMETHOD_SINGLECWD)));
      require_curl(curl_easy_setopt(easy, CURLOPT_PATH_AS_IS, 1L));
      require_curl(curl_easy_setopt(easy, CURLOPT_TRANSFERTEXT, 0L));
      require_curl(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L));
      require_curl(curl_easy_setopt(easy, CURLOPT_SERVER_RESPONSE_TIMEOUT, 30L));
      require_curl(curl_easy_setopt(easy, CURLOPT_ACCEPTTIMEOUT_MS, 30000L));
      require_curl(curl_easy_setopt(easy, CURLOPT_UPLOAD, op->request.upload ? 1L : 0L));
      require_curl(curl_easy_setopt(easy, CURLOPT_NOBODY, op->request.no_body ? 1L : 0L));
      require_curl(curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST,
          op->request.custom_request.empty() ? nullptr : op->request.custom_request.c_str()));
      op->quote = make_commands(op->request.quote);
      op->prequote = make_commands(op->request.prequote);
      require_curl(curl_easy_setopt(easy, CURLOPT_QUOTE, op->quote));
      require_curl(curl_easy_setopt(easy, CURLOPT_PREQUOTE, op->prequote));
      require_curl(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive));
      require_curl(curl_easy_setopt(easy, CURLOPT_WRITEDATA, this));
      require_curl(curl_easy_setopt(easy, CURLOPT_READFUNCTION, send));
      require_curl(curl_easy_setopt(easy, CURLOPT_READDATA, this));
      require_curl(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header));
      require_curl(curl_easy_setopt(easy, CURLOPT_HEADERDATA, this));
      require_curl(curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress));
      require_curl(curl_easy_setopt(easy, CURLOPT_XFERINFODATA, this));
      require_curl(curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L));
      require_curl(curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, configure_socket));
      require_curl(curl_easy_setopt(easy, CURLOPT_SOCKOPTDATA, this));
      require_curl(curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, op->error.data()));
      require_curl(curl_easy_setopt(easy, CURLOPT_CLOSESOCKETFUNCTION, close_socket));
      require_curl(curl_easy_setopt(easy, CURLOPT_CLOSESOCKETDATA, this));
      require_curl(curl_easy_setopt(easy, CURLOPT_MAXCONNECTS, 1L));
      result.code = curl_easy_perform(easy);
      result.error = op->error.data();
      if (certificates) {
        result.certificate_failure = certificates->failure;
        if (certificates->callback_failure) op->callback_failure = certificates->callback_failure;
      }
      require_curl(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.response_code));
      char *entry = nullptr;
      require_curl(curl_easy_getinfo(easy, CURLINFO_FTP_ENTRY_PATH, &entry));
      if (entry != nullptr) result.entry_path = entry;
    } catch (...) {
      failure = std::current_exception();
    }
    // Cleanup may still invoke callbacks. Only worker-owned user data remains
    // installed after the request's command lists and error buffer are released.
    if (easy) {
      curl_easy_setopt(easy, CURLOPT_QUOTE, nullptr);
      curl_easy_setopt(easy, CURLOPT_PREQUOTE, nullptr);
      curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, nullptr);
      if (result.certificate_failure) {
        // Recreate the transport before a retry, keeping only this logical
        // session's exact approvals. A failed mutation is never replayed here.
        curl_easy_cleanup(easy);
        easy = nullptr;
      }
    }
    {
      const auto lock = std::lock_guard(socket_mutex);
      operation.reset();
    }
    if (stopping.load() || op->cancellation.is_cancellation_requested()) {
      op->completion.try_cancel();
    } else if (failure || op->callback_failure) {
      op->completion.try_reject(failure ? failure : op->callback_failure);
    } else {
      op->completion.try_resolve(std::move(result));
    }
  }

  void run() noexcept {
    try {
      easy = curl_easy_init();
      if (easy == nullptr) throw std::bad_alloc();
      ready.try_resolve();
      for (;;) {
        std::shared_ptr<CurlOperation> next;
        {
          auto lock = std::unique_lock(mutex);
          changed.wait(lock, [&] { return stopping.load() || !pending.empty(); });
          if (stopping.load()) break;
          next = std::move(pending.front());
          pending.pop_front();
        }
        perform(next);
      }
    } catch (...) {
      ready.try_reject(std::current_exception());
    }
    stopping.store(true);
    std::deque<std::shared_ptr<CurlOperation>> remaining;
    {
      const auto lock = std::lock_guard(mutex);
      remaining.swap(pending);
    }
    for (const auto &op : remaining) op->completion.try_cancel();
    if (easy != nullptr) curl_easy_cleanup(easy);
    easy = nullptr;
  }
};

// start_new reports thread-start failures through its returned promise. Also
// settle initialization in that case, so the factory cannot wait indefinitely.
static cardio::promise<void> observe_worker_async(
    std::shared_ptr<CurlWorker> worker, cardio::promise<void> task) {
  try {
    co_await task;
  } catch (...) {
    worker->ready.try_reject(std::current_exception());
    throw;
  }
}


// The cached connection's QUIT uses an internal curl handle without our
// progress callback. Bound graceful shutdown with a caller-owned timer. Only
// socket shutdown crosses threads; curl performs close and cleanup itself.
static cardio::promise<void> bound_shutdown_async(
    std::shared_ptr<CurlWorker> worker, cardio::cancellation cancellation) {
  try {
    co_await cardio::promises::delay(1000, cancellation);
    worker->shutdown_sockets();
  } catch (const cardio::canceled_exception &) {
  }
}

struct CurlOwnedDescriptor {
  int fd = -1;

  CurlOwnedDescriptor() = default;
  explicit CurlOwnedDescriptor(int fd) : fd(fd) {}
  CurlOwnedDescriptor(CurlOwnedDescriptor &&other) noexcept
      : fd(std::exchange(other.fd, -1)) {}
  CurlOwnedDescriptor(const CurlOwnedDescriptor &) = delete;
  CurlOwnedDescriptor &operator=(const CurlOwnedDescriptor &) = delete;
  ~CurlOwnedDescriptor() { reset(); }

  void reset() noexcept {
    if (fd >= 0) (void)::close(std::exchange(fd, -1));
  }
};

class CurlFtpSessionAdapter final : public CurlFtpSession {
  std::shared_ptr<CurlWorker> worker;
  CurlOwnedDescriptor thread_exit_read;
  cardio::promise<void> worker_task;
  cardio::primitives::mutex stop_mutex;
  bool stopped = false;
  cardio::cancellation_source confirmation_stop;
  std::optional<FtpCertificateFailure> pending_approval;

public:
  explicit CurlFtpSessionAdapter(std::shared_ptr<CurlWorker> worker)
      : worker(std::move(worker)) {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
      throw std::system_error(errno, std::generic_category(), "FTP worker exit pipe");
    }
    thread_exit_read.fd = descriptors[0];
    worker_task = observe_worker_async(this->worker,
        cardio::promises::start_new(
            [state = this->worker,
             exit_writer = CurlOwnedDescriptor(descriptors[1])]() mutable {
              // start_new resolves before destroying its local dispatcher.
              // Thread-local destruction follows that teardown and releases
              // the pipe only after the callable's captured state is gone.
              thread_local auto exit_signal = std::move(exit_writer);
              (void)exit_signal;
              state->run();
            }));
  }

  ~CurlFtpSessionAdapter() override {
    (void)confirmation_stop.cancel();
    worker->request_stop();
    worker->shutdown_sockets();
  }

  cardio::promise<CurlFtpResult> perform_async(
      CurlFtpRequest request, cardio::cancellation cancellation) override {
    auto combined = cardio::cancellations::any(cancellation, confirmation_stop.get_cancellation());
    cancellation = combined.get_cancellation();
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      auto op = std::make_shared<CurlOperation>();
      op->request = request;
      op->certificate_approval = std::exchange(pending_approval, std::nullopt);
      op->cancellation = cancellation;
      auto completed = op->completion.get_promise();
      auto registration = cancellation.on_cancellation_requested(
          [state = worker, op] { state->cancel(op); });
      worker->submit(op);
      auto result = co_await completed;
      if (!result.certificate_failure || !worker->options.confirm_certificate) co_return result;
      const auto accepted = co_await worker->options.confirm_certificate(*result.certificate_failure, cancellation);
      cancellation.throw_if_cancellation_requested();
      if (!accepted) co_return result;
      pending_approval = result.certificate_failure;
      if (request.initial_authentication && result.certificate_failure->channel == FtpTlsChannel::control) continue;
      result.certificate_accepted = true;
      result.error += "; certificate accepted for this connection; the failed operation was not retried";
      co_return result;
    }
  }

  void resume() override { worker->wake(); }

  cardio::promise<void> stop_async() override {
    (void)confirmation_stop.cancel();
    worker->request_stop();
    auto lock = std::move(co_await stop_mutex.lock());
    if (!stopped) {
      cardio::cancellation_source timer_cancel;
      auto deadline = bound_shutdown_async(worker, timer_cancel.get_cancellation());
      std::exception_ptr failure;
      try {
        co_await worker_task;
      } catch (...) {
        failure = std::current_exception();
      }
      // No bytes are written: hangup signals worker dispatcher teardown.
      // A failed thread start also closes the callable's owned writer.
      co_await cardio::from_fd(thread_exit_read.fd, cardio::fd_event::read);
      thread_exit_read.reset();
      (void)timer_cancel.cancel();
      co_await deadline;
      stopped = true;
      if (failure) std::rethrow_exception(failure);
    }
  }
};

cardio::promise<std::shared_ptr<CurlFtpSession>>
open_curl_ftp_session_async(FtpClientOpenOptions options) {
  if (!options.connection.validation_errors.empty())
    throw std::invalid_argument(options.connection.validation_errors.front());
  static const CurlGlobal global;
  (void)global;
  const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
  if (version->version_num < 0x075801 || version->age < CURLVERSION_ELEVENTH) {
    throw std::runtime_error("FTP requires libcurl 7.88.1 or newer");
  }
  bool asynchronous_dns = false;
  bool ssl = false;
  for (const char *const *feature = version->feature_names; feature && *feature; ++feature) {
    ssl = ssl || std::string_view(*feature) == "SSL";
    asynchronous_dns = asynchronous_dns || std::string_view(*feature) == "AsynchDNS";
  }
  if (!asynchronous_dns) {
    throw std::runtime_error("FTP requires libcurl with asynchronous DNS");
  }
  if (options.connection.tls_mode != FtpTlsMode::none && !ssl)
    throw std::runtime_error("FTPS requires libcurl with TLS support");
  bool ftp = false;
  bool ftps = false;
  for (const char *const *protocol = version->protocols; *protocol; ++protocol) {
    ftps = ftps || std::string_view(*protocol) == "ftps";
    ftp = ftp || std::string_view(*protocol) == "ftp";
  }
  if (!ftp) throw std::runtime_error("libcurl was built without FTP support");
  if (options.connection.tls_mode == FtpTlsMode::implicit_tls && !ftps)
    throw std::runtime_error("libcurl was built without FTPS support");
  if (options.connection.tls_mode != FtpTlsMode::none &&
      (options.connection.tls_compatibility == FtpTlsCompatibility::openssl_legacy ||
       !options.connection.tls_cipher_list.empty() || !options.connection.tls13_cipher_list.empty()) &&
      (!version->ssl_version || !std::string_view(version->ssl_version).starts_with("OpenSSL/")))
    throw std::runtime_error("These FTPS cipher and compatibility settings require OpenSSL");
  auto worker = std::make_shared<CurlWorker>(std::move(options));
  auto ready = worker->ready.get_promise();
  auto session = std::make_shared<CurlFtpSessionAdapter>(worker);
  std::exception_ptr failure;
  try {
    co_await ready;
  } catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    co_await session->stop_async();
    std::rethrow_exception(failure);
  }
  co_return session;
}

} // namespace elder_terms
