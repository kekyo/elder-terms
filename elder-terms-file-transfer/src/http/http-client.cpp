// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include "http-client.h"
#include "curl-http-stream.h"
#include <array>
#include <set>

namespace elder_terms {

struct HttpConnection {
  HttpClientOpenOptions options;
  std::shared_ptr<CurlHttpSession> session;
  std::map<std::string, std::shared_ptr<HttpCrawlerPage>> pages;
  std::set<std::string> visited;
  bool transferring = false;
};

static std::string resource_url(const HttpConnection &connection, const std::string &path) {
  const auto url = http_resolve_url(connection.options.base_url, path);
  if (!http_url_in_scope(connection.options.base_url, url))
    throw std::invalid_argument("HTTP URL is outside the configured base URL");
  return url;
}

static void cache_page(HttpConnection &connection, HttpCrawlerPage page) {
  auto stored = std::make_shared<HttpCrawlerPage>(std::move(page));
  stored->directory.resource_type = stored->resource.type;
  for (const auto &alias : stored->aliases) connection.pages[alias] = stored;
}

static cardio::promise<RemoteDirectorySnapshot> browse_async(
    std::shared_ptr<HttpConnection> connection, std::string path, bool refresh,
    bool force_html, cardio::cancellation cancellation) {
  const auto url = resource_url(*connection, path);
  cancellation.throw_if_cancellation_requested();
  if (refresh || force_html || !connection->pages.contains(url)) {
    const auto page = co_await fetch_http_page_async(connection->session, connection->options.base_url,
        url, force_html, connection->options.limits, cancellation);
    cache_page(*connection, page);
  }
  auto directory = connection->pages.at(url)->directory;
  for (auto &entry : directory.entries) {
    const auto known = connection->pages.find(entry.path);
    if (known == connection->pages.end()) continue;
    auto attributes = known->second->resource;
    attributes.path = entry.path;
    attributes.name = entry.name;
    entry = std::move(attributes);
  }
  co_return directory;
}

struct HttpAlreadyVisited {};

static HttpReadOptions read_options(std::shared_ptr<HttpConnection> connection,
    std::string url, std::shared_ptr<std::vector<std::string>> aliases, bool deduplicate) {
  aliases->push_back(url);
  HttpReadOptions options;
  options.url = std::move(url);
  options.redirect_limit = connection->options.limits.redirects;
  options.redirect = [connection, aliases, deduplicate](const std::string &current, const std::string &location) {
    const auto next = resource_url(*connection, http_resolve_url(current, location));
    if (std::find(aliases->begin(), aliases->end(), next) != aliases->end())
      throw std::runtime_error("HTTP redirect cycle");
    if (deduplicate && connection->visited.contains(next)) throw HttpAlreadyVisited{};
    aliases->push_back(next);
    return next;
  };
  return options;
}

static cardio::promise<std::optional<RemoteReceiveSource>> prepare_async(
    std::shared_ptr<HttpConnection> connection, std::string path, bool as_file,
    cardio::cancellation cancellation) {
  const auto url = resource_url(*connection, path);
  cancellation.throw_if_cancellation_requested();
  if (connection->visited.contains(url)) co_return std::nullopt;
  const auto cached = connection->pages.find(url);
  if (!as_file && cached != connection->pages.end() && cached->second->html) {
    const auto &page = *cached->second;
    connection->visited.insert(page.aliases.begin(), page.aliases.end());
    co_return RemoteReceiveSource{page.resource, page.directory.entries, {}};
  }
  auto aliases = std::make_shared<std::vector<std::string>>();
  auto options = read_options(connection, url, aliases, true);
  auto response = std::make_shared<CurlHttpResult>();
  auto final_url = std::make_shared<std::string>(url);
  options.response = [response, final_url](const std::string &target, const CurlHttpResult &headers) {
    *response = headers; *final_url = target;
  };
  std::unique_ptr<RemoteFileReader> reader;
  try { reader = std::move(co_await open_http_reader_async(connection->session, std::move(options), cancellation)); }
  catch (const HttpAlreadyVisited &) {
    // The final resource was already visited, but the redirect chain also
    // belongs to this job. Remember it before another page repeats the alias.
    connection->visited.insert(aliases->begin(), aliases->end());
    co_return std::nullopt;
  }
  HttpCrawlerPage page;
  std::exception_ptr failure;
  const bool html = !as_file && http_response_is_html(*response);
  try {
    std::string body;
    if (html) {
      std::array<std::byte, 65536> buffer;
      for (;;) {
        const auto count = co_await reader->read_async(buffer, cancellation);
        if (!count) break;
        if (count > connection->options.limits.html_bytes - body.size())
          throw std::runtime_error("HTTP HTML body limit exceeded (2 MiB)");
        body.append(reinterpret_cast<const char *>(buffer.data()), count);
      }
      co_await reader->close_async(cancellation);
      reader.reset();
    }
    auto metadata = *response;
    if (as_file) metadata.headers["content-type"] = "application/octet-stream";
    page = parse_http_page(connection->options.base_url, *final_url, metadata, body,
        *aliases, false, connection->options.limits);
  } catch (...) { failure = std::current_exception(); }
  if (failure) {
    if (reader) co_await reader->close_async({});
    std::rethrow_exception(failure);
  }
  connection->visited.insert(aliases->begin(), aliases->end());
  if (!as_file) cache_page(*connection, page);
  co_return RemoteReceiveSource{page.resource, page.directory.entries, std::move(reader)};
}

static void reject_write() { throw RemoteFileError("HTTP connections are read-only", false); }

// This adapter implements the existing remote-service interface; all state and
// asynchronous operations are delegated to the owned connection above.
class HttpClient final : public RemoteFileClient {
public:
  std::shared_ptr<HttpConnection> connection;
  std::string display_path(const std::string &path) const override {
    const auto url = resource_url(*connection, path);
    return url.substr(url.find('/', url.find("://") + 3));
  }
  cardio::promise<std::string> resolve_input_path_async(
      std::string text, cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    if (text.starts_with('/')) {
      const auto &base = connection->options.base_url;
      // An entered absolute path always belongs to this connection, including
      // paths beginning with two slashes that URL resolution treats as a host.
      text = base.substr(0, base.find('/', base.find("://") + 3)) + text;
    }
    co_return resource_url(*connection, text);
  }
  std::string parent_path(const std::string &path) const override {
    const auto url_path = path.substr(0, path.find('?'));
    return http_resolve_url(path, url_path.ends_with('/') ? "../" : "./");
  }
  RemoteFileCapabilities capabilities() const noexcept override {
    return {.write_operations = false, .incremental_receive = true};
  }
  RemoteReceiveLimits receive_limits() const override {
    const auto &limits = connection->options.limits;
    return {limits.depth, limits.pages, limits.links};
  }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(std::string path, cardio::cancellation cancellation) override {
    return browse_async(connection, std::move(path), false, false, cancellation);
  }
  cardio::promise<RemoteDirectorySnapshot> browse_directory_async(
      std::string path, bool refresh, bool force_html, cardio::cancellation cancellation) override {
    return browse_async(connection, std::move(path), refresh, force_html, cancellation);
  }
  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(std::string path, cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto url = resource_url(*connection, path);
    const auto found = connection->pages.find(url);
    if (found != connection->pages.end()) co_return found->second->resource;
    RemoteFileAttributes attributes;
    attributes.path = url; attributes.name = http_resource_name(url); attributes.type = RemoteFileType::unresolved;
    co_return attributes;
  }
  cardio::promise<std::optional<RemoteReceiveSource>> prepare_receive_async(
      std::string path, bool as_file, cardio::cancellation cancellation) override {
    return prepare_async(connection, std::move(path), as_file, cancellation);
  }
  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(std::string path, cardio::cancellation cancellation) override {
    auto aliases = std::make_shared<std::vector<std::string>>();
    co_return std::move(co_await open_http_reader_async(connection->session,
        read_options(connection, resource_url(*connection, path), aliases, false), cancellation));
  }
  cardio::promise<std::string> read_link_async(std::string, cardio::cancellation) override { reject_write(); co_return ""; }
  cardio::promise<void> make_directory_async(std::string, std::optional<std::uint32_t>, cardio::cancellation) override { reject_write(); co_return; }
  cardio::promise<void> remove_file_async(std::string, cardio::cancellation) override { reject_write(); co_return; }
  cardio::promise<void> remove_directory_async(std::string, cardio::cancellation) override { reject_write(); co_return; }
  cardio::promise<void> rename_async(std::string, std::string, cardio::cancellation) override { reject_write(); co_return; }
  cardio::promise<void> make_symbolic_link_async(std::string, std::string, cardio::cancellation) override { reject_write(); co_return; }
  cardio::promise<void> set_attributes_async(std::string, RemoteFileAttributes, cardio::cancellation) override { reject_write(); co_return; }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(std::string, std::uint64_t, std::optional<std::uint32_t>, cardio::cancellation) override { reject_write(); co_return nullptr; }
  bool try_begin_transfer() override {
    if (connection->transferring) return false;
    connection->visited.clear(); connection->transferring = true; return true;
  }
  void end_transfer() override { connection->transferring = false; connection->visited.clear(); }
};

cardio::promise<std::shared_ptr<RemoteFileClient>> open_http_client_async(
    HttpClientOpenOptions options, cardio::cancellation cancellation) {
  options.base_url = http_scope_url(options.base_url);
  options.start_url = http_resolve_url(options.base_url, options.start_url);
  if (!http_url_in_scope(options.base_url, options.start_url)) throw std::invalid_argument("HTTP start page is outside the base URL");
  auto connection = std::make_shared<HttpConnection>();
  connection->session = create_curl_http_session(options.transport, options.password, options.confirm_certificate, options.proxy);
  connection->options = std::move(options);
  std::exception_ptr failure;
  try {
    const auto initial = co_await browse_async(connection, connection->options.start_url, false, false, cancellation);
    if (initial.resource_type != RemoteFileType::directory) throw std::runtime_error("HTTP start page is not HTML");
  } catch (...) { failure = std::current_exception(); }
  if (failure) { co_await stop_curl_http_session_async(connection->session); std::rethrow_exception(failure); }
  auto client = std::make_shared<HttpClient>(); client->connection = std::move(connection);
  co_return client;
}

cardio::promise<void> stop_http_client_async(std::shared_ptr<RemoteFileClient> client) {
  const auto http = std::dynamic_pointer_cast<HttpClient>(client);
  if (http) co_await stop_curl_http_session_async(http->connection->session);
}
}
