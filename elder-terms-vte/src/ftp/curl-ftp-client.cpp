#include "curl-ftp-client.h"
#include "curl-ftp-session.h"
#include "ftp-protocol.h"

#include <glib.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace elder_terms {

static constexpr std::size_t control_limit = 64 * 1024;
static constexpr std::size_t listing_limit = 64 * 1024 * 1024;

static void validate_argument(const std::string &value, const char *name,
                              bool allow_empty) {
  if ((!allow_empty && value.empty()) || !ftp_command_argument_is_safe(value)) {
    throw std::invalid_argument(std::string("Invalid FTP ") + name);
  }
}

static std::string_view trim_line(std::string_view line) {
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    line.remove_prefix(1);
  }
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                           line.back() == ' ' || line.back() == '\t')) {
    line.remove_suffix(1);
  }
  return line;
}

static std::string path_name(const std::string &path) {
  return path.substr(path.find_last_of('/') + 1);
}

// Preserve dot components until the server resolves them. Local lexical
// normalization would change paths traversing server-side directory aliases.
static std::string absolute_path(const std::string &directory,
                                 const std::string &path) {
  auto result = path.empty() ? directory : path.front() == '/' ? path
      : directory + (directory.ends_with('/') ? "" : "/") + path;
  while (result.size() > 1 && result.ends_with('/')) result.pop_back();
  return result;
}

static std::string encode_path(const std::string &path) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char ch : path) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '/' || ch == '-' || ch == '_' ||
        ch == '.' || ch == '~') {
      result += static_cast<char>(ch);
    } else {
      result += '%';
      result += hex[ch >> 4];
      result += hex[ch & 15];
    }
  }
  return result;
}

static std::string endpoint_url(const FtpConnectionSettings &connection) {
  const auto url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(
      curl_url(), curl_url_cleanup);
  if (!url) throw std::bad_alloc();
  const auto set = [&](CURLUPart part, const std::string &value) {
    const auto code = curl_url_set(url.get(), part, value.c_str(), 0);
    if (code != CURLUE_OK) {
      throw std::invalid_argument(std::string("Invalid FTP endpoint: ") +
                                  curl_url_strerror(code));
    }
  };
  set(CURLUPART_SCHEME, "ftp");
  const auto &address = connection.address;
  set(CURLUPART_HOST, address.find(':') != std::string::npos &&
                         !address.starts_with('[') ? "[" + address + "]" : address);
  set(CURLUPART_PORT, std::to_string(connection.port));
  set(CURLUPART_PATH, "/");
  char *text = nullptr;
  const auto code = curl_url_get(url.get(), CURLUPART_URL, &text, 0);
  const auto owned = std::unique_ptr<char, decltype(&curl_free)>(text, curl_free);
  if (code != CURLUE_OK) {
    throw std::invalid_argument(std::string("Invalid FTP endpoint: ") +
                                curl_url_strerror(code));
  }
  return text;
}

struct ControlLines {
  std::size_t size = 0;
  std::vector<std::string> lines;

  void receive(std::string_view line) {
    if (line.size() > control_limit - size) {
      throw std::runtime_error("FTP control response exceeds its limit");
    }
    size += line.size();
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
  }

  bool feature(std::string_view name) const {
    return std::any_of(lines.begin(), lines.end(), [&](const auto &text) {
      const auto line = trim_line(text);
      return line.size() >= name.size() &&
          g_ascii_strncasecmp(line.data(), name.data(), name.size()) == 0 &&
          (line.size() == name.size() || line[name.size()] == ' ');
    });
  }

  std::string working_directory() const {
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
      if (line->starts_with("257 ")) {
        auto path = parse_ftp_pwd_path({.code = 257, .lines = {*line}});
        validate_argument(path, "server directory", false);
        if (!path.starts_with('/')) {
          throw std::runtime_error("FTP server returned a nonabsolute directory");
        }
        return absolute_path("/", path);
      }
    }
    throw std::runtime_error("FTP PWD response contained no directory");
  }
};

static bool command_unavailable(long code) {
  return code == 500 || code == 501 || code == 502 || code == 504 || code == 522;
}

static bool ordinary_refusal(CURLcode code) {
  return code == CURLE_QUOTE_ERROR || code == CURLE_REMOTE_ACCESS_DENIED ||
      code == CURLE_REMOTE_FILE_NOT_FOUND || code == CURLE_FTP_COULDNT_RETR_FILE ||
      code == CURLE_UPLOAD_FAILED;
}

static std::runtime_error operation_error(const char *name,
                                         const CurlFtpResult &result) {
  return std::runtime_error(std::string(name) + " failed (FTP " +
      std::to_string(result.response_code) + ", curl " +
      std::to_string(result.code) + "): " +
      (result.error.empty() ? curl_easy_strerror(result.code) : result.error));
}

static void require_success(const char *name, const CurlFtpResult &result) {
  if (result.code != CURLE_OK) throw operation_error(name, result);
}

static RemoteFileAttributes attributes(const FtpDirectoryEntry &entry,
                                       std::string path, std::string name) {
  return {.name = std::move(name), .path = std::move(path),
          .type = entry.type == FtpDirectoryEntryType::regular
              ? RemoteFileType::regular
              : entry.type == FtpDirectoryEntryType::directory
                  ? RemoteFileType::directory : RemoteFileType::other,
          .size = entry.size, .permissions = std::nullopt,
          .access_time_unix_seconds = std::nullopt,
          .modification_time_unix_seconds = entry.modification_time_unix_seconds};
}

static RemoteDirectorySnapshot directory_snapshot(
    std::string directory, const std::string &listing, bool machine_readable) {
  RemoteDirectorySnapshot result{.canonical_path = std::move(directory), .entries = {}};
  std::size_t offset = 0;
  while (offset < listing.size()) {
    const auto end = listing.find('\n', offset);
    const auto length = (end == std::string::npos ? listing.size() : end) - offset;
    if (length > control_limit) throw std::runtime_error("FTP listing line is too long");
    auto line = std::string_view(listing).substr(offset, length);
    if (line.ends_with('\r')) line.remove_suffix(1);
    offset += length + 1;
    const auto entry = machine_readable ? parse_ftp_mlsd_entry(line)
                                       : parse_ftp_list_entry(line);
    if (!entry) continue;
    if (entry->type == FtpDirectoryEntryType::current_directory ||
        entry->type == FtpDirectoryEntryType::parent_directory ||
        entry->name == "." || entry->name == "..") continue;
    const auto &name = entry->name;
    if (name.empty() || name.find('/') != std::string::npos ||
        !ftp_command_argument_is_safe(name) ||
        !g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr)) {
      throw std::runtime_error("FTP server returned an invalid directory entry name");
    }
    result.entries.push_back(attributes(*entry, absolute_path(result.canonical_path, name), name));
  }
  std::sort(result.entries.begin(), result.entries.end(),
            [](const auto &left, const auto &right) { return left.name < right.name; });
  return result;
}

// State used by the caller dispatcher. Only request callbacks access their
// private response buffers on the worker, until perform_async has completed.
struct CurlClientState {
  std::shared_ptr<CurlFtpSession> session;
  std::string base_url;
  std::string directory;
  cardio::primitives::mutex operations;
  bool has_mlsd = false;
  bool has_mlst = false;
  bool failed = false;
  bool stopping = false;
  bool transferring = false;

  void require_open() const {
    if (stopping || failed) throw std::runtime_error("FTP session is closed");
  }

  std::string file_url(const std::string &path, bool is_directory) const {
    // The extra slash selects the server root instead of the login directory.
    auto result = base_url + encode_path(path);
    if (is_directory && !result.ends_with('/')) result += '/';
    return result;
  }

  cardio::promise<CurlFtpResult> perform_async(
      CurlFtpRequest request, cardio::cancellation cancellation) {
    require_open();
    CurlFtpResult result;
    try {
      result = co_await session->perform_async(std::move(request), cancellation);
    } catch (...) {
      failed = true;
      throw;
    }
    if (result.code != CURLE_OK && !ordinary_refusal(result.code)) failed = true;
    co_return result;
  }

  cardio::promise<CurlFtpResult> command_async(
      std::vector<std::string> commands, std::shared_ptr<ControlLines> response,
      cardio::cancellation cancellation) {
    CurlFtpRequest request;
    request.url = base_url;
    request.quote = std::move(commands);
    request.no_body = true;
    request.header = [response](std::string_view line) { response->receive(line); };
    co_return co_await perform_async(std::move(request), cancellation);
  }

  cardio::promise<RemoteDirectorySnapshot> list_async(
      std::string path, cardio::cancellation cancellation) {
    for (;;) {
      auto headers = std::make_shared<ControlLines>();
      CurlFtpRequest probe;
      probe.url = file_url(path, true);
      probe.no_body = true;
      probe.prequote = {"PWD"};
      probe.header = [headers](std::string_view line) { headers->receive(line); };
      const auto inspected = co_await perform_async(std::move(probe), cancellation);
      require_success("FTP directory location", inspected);
      const auto canonical = headers->working_directory();
      auto listing = std::make_shared<std::string>();
      CurlFtpRequest request;
      request.url = file_url(path, true);
      request.custom_request = has_mlsd ? "MLSD" : "LIST";
      request.receive = [listing](std::span<const std::byte> bytes) {
        if (bytes.size() > listing_limit - listing->size()) {
          throw std::runtime_error("FTP directory listing exceeds its limit");
        }
        listing->append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return bytes.size();
      };
      const auto result = co_await perform_async(std::move(request), cancellation);
      if (has_mlsd && command_unavailable(result.response_code) &&
          result.code == CURLE_FTP_COULDNT_RETR_FILE) {
        has_mlsd = false;
        continue;
      }
      require_success("FTP directory listing", result);
      // Curl can return CURLE_OK for LIST 450 without receiving a listing.
      // An empty directory still requires a successful final transfer reply.
      if (result.response_code != 226 && result.response_code != 250) {
        throw operation_error("FTP directory listing", result);
      }
      co_return directory_snapshot(canonical, *listing, has_mlsd);
    }
  }

  cardio::promise<std::optional<RemoteFileAttributes>> stat_async(
      std::string path, cardio::cancellation cancellation) {
    std::optional<CurlFtpResult> refusal;
    if (has_mlst) {
      auto response = std::make_shared<ControlLines>();
      std::vector<std::string> commands{"MLST " + path};
      const auto result = co_await command_async(std::move(commands), response, cancellation);
      if (result.code == CURLE_OK) {
        for (const auto &line : response->lines) {
          const auto parsed = parse_ftp_mlsd_entry(trim_line(line));
          if (parsed) co_return attributes(*parsed, path, path_name(path));
        }
        throw std::runtime_error("FTP MLST response contained no facts");
      }
      if (command_unavailable(result.response_code) && result.code == CURLE_QUOTE_ERROR) {
        has_mlst = false;
      } else if (result.response_code == 550 && result.code == CURLE_QUOTE_ERROR) {
        refusal = result;
      } else {
        throw operation_error("FTP MLST", result);
      }
    }
    if (path == "/") {
      if (refusal) throw operation_error("FTP MLST", *refusal);
      throw std::runtime_error("FTP server does not support MLST for the root item");
    }
    const auto slash = path.find_last_of('/');
    const auto parent = slash == 0 ? "/" : path.substr(0, slash);
    const auto name = path_name(path);
    const auto snapshot = co_await list_async(parent, cancellation);
    const auto found = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
                                   [&name](const auto &entry) { return entry.name == name; });
    if (found == snapshot.entries.end()) co_return std::nullopt;
    if (refusal) throw operation_error("FTP MLST", *refusal);
    auto result = *found;
    result.path = path;
    co_return result;
  }
};

class CurlFileClient final : public RemoteFileClient {
  std::shared_ptr<CurlClientState> state;

  cardio::promise<void> mutate_async(std::string verb, std::string path,
                                    cardio::cancellation cancellation) {
    validate_argument(path, "path", false);
    const auto state = this->state;
    auto lock = std::move(co_await state->operations.lock(cancellation));
    state->require_open();
    auto response = std::make_shared<ControlLines>();
    std::vector<std::string> commands{verb + " " + absolute_path(state->directory, path)};
    const auto result = co_await state->command_async(std::move(commands), response, cancellation);
    require_success(("FTP " + verb).c_str(), result);
  }

public:
  explicit CurlFileClient(std::shared_ptr<CurlClientState> state)
      : state(std::move(state)) {}

  RemoteFileCapabilities capabilities() const noexcept override { return {}; }

  cardio::promise<void> stop_async() {
    const auto state = this->state;
    state->stopping = true;
    co_await state->session->stop_async();
    // Waiters fail require_open() in FIFO order before dispatcher destruction.
    auto lock = std::move(co_await state->operations.lock());
  }

  cardio::promise<RemoteDirectorySnapshot> load_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_argument(path, "path", true);
    const auto state = this->state;
    auto lock = std::move(co_await state->operations.lock(cancellation));
    state->require_open();
    auto result = co_await state->list_async(absolute_path(state->directory, path), cancellation);
    state->directory = result.canonical_path;
    co_return result;
  }

  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(
      std::string path, cardio::cancellation cancellation) override {
    validate_argument(path, "path", false);
    const auto state = this->state;
    auto lock = std::move(co_await state->operations.lock(cancellation));
    state->require_open();
    co_return co_await state->stat_async(absolute_path(state->directory, path), cancellation);
  }

  cardio::promise<void> make_directory_async(
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    (void)permissions;
    return mutate_async("MKD", std::move(path), cancellation);
  }

  cardio::promise<void> remove_file_async(
      std::string path, cardio::cancellation cancellation) override {
    return mutate_async("DELE", std::move(path), cancellation);
  }

  cardio::promise<void> remove_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    return mutate_async("RMD", std::move(path), cancellation);
  }

  cardio::promise<void> rename_async(
      std::string source, std::string destination,
      cardio::cancellation cancellation) override {
    validate_argument(source, "source path", false);
    validate_argument(destination, "destination path", false);
    const auto state = this->state;
    auto lock = std::move(co_await state->operations.lock(cancellation));
    state->require_open();
    auto response = std::make_shared<ControlLines>();
    std::vector<std::string> commands{
        "RNFR " + absolute_path(state->directory, source),
        "RNTO " + absolute_path(state->directory, destination)};
    const auto result = co_await state->command_async(std::move(commands), response, cancellation);
    require_success("FTP rename", result);
  }

  cardio::promise<std::string> read_link_async(
      std::string path, cardio::cancellation cancellation) override {
    (void)path;
    cancellation.throw_if_cancellation_requested();
    throw std::runtime_error("FTP symbolic links are unsupported");
    co_return std::string{};
  }

  cardio::promise<void> make_symbolic_link_async(
      std::string target, std::string path, cardio::cancellation cancellation) override {
    (void)target;
    (void)path;
    cancellation.throw_if_cancellation_requested();
    throw std::runtime_error("FTP symbolic links are unsupported");
    co_return;
  }

  cardio::promise<void> set_attributes_async(
      std::string path, RemoteFileAttributes value,
      cardio::cancellation cancellation) override {
    (void)path;
    (void)value;
    cancellation.throw_if_cancellation_requested();
    co_return;
  }

  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(
      std::string path, cardio::cancellation cancellation) override {
    (void)path;
    cancellation.throw_if_cancellation_requested();
    throw std::runtime_error("libcurl FTP streaming is not implemented");
    co_return nullptr;
  }

  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    (void)path;
    (void)permissions;
    cancellation.throw_if_cancellation_requested();
    throw std::runtime_error("libcurl FTP streaming is not implemented");
    co_return nullptr;
  }

  bool try_begin_transfer() override {
    if (state->transferring || state->failed || state->stopping) return false;
    state->transferring = true;
    return true;
  }

  void end_transfer() override { state->transferring = false; }
};

cardio::promise<std::shared_ptr<RemoteFileClient>> open_libcurl_ftp_client_async(
    FtpClientOpenOptions options, cardio::cancellation cancellation) {
  validate_argument(options.connection.username, "username", false);
  validate_argument(options.password, "password", true);
  validate_argument(options.connection.address, "address", false);
  if (options.connection.port <= 0 || options.connection.port > 65535) {
    throw std::invalid_argument("Invalid FTP port");
  }
  cancellation.throw_if_cancellation_requested();
  auto state = std::make_shared<CurlClientState>();
  state->session = co_await open_curl_ftp_session_async(options);
  std::exception_ptr failure;
  try {
    state->base_url = endpoint_url(options.connection);
    auto features = std::make_shared<ControlLines>();
    std::vector<std::string> commands{"*FEAT"};
    const auto result = co_await state->command_async(std::move(commands), features, cancellation);
    require_success("FTP login", result);
    validate_argument(result.entry_path, "login directory", false);
    if (!result.entry_path.starts_with('/')) {
      throw std::runtime_error("FTP server returned a nonabsolute login directory");
    }
    state->directory = absolute_path("/", result.entry_path);
    state->has_mlst = features->feature("MLST");
    state->has_mlsd = state->has_mlst || features->feature("MLSD");
    if (features->feature("UTF8")) {
      auto response = std::make_shared<ControlLines>();
      commands = {"*OPTS UTF8 ON"};
      const auto configured = co_await state->command_async(std::move(commands), response, cancellation);
      require_success("FTP UTF8", configured);
    }
  } catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    co_await state->session->stop_async();
    std::rethrow_exception(failure);
  }
  co_return std::make_shared<CurlFileClient>(std::move(state));
}

cardio::promise<void> stop_libcurl_ftp_client_async(std::shared_ptr<RemoteFileClient> client) {
  auto ftp = std::dynamic_pointer_cast<CurlFileClient>(client);
  if (!ftp) throw std::invalid_argument("Expected a libcurl FTP client");
  co_await ftp->stop_async();
}

} // namespace elder_terms
