#include "webdav-client.h"
#include "curl-http-session.h"
#include "webdav-metadata.h"
#include "webdav-stream.h"

#include <algorithm>
#include <stdexcept>

namespace elder_terms {


static cardio::promise<void> unavailable() {
  throw std::runtime_error("This WebDAV operation is not available yet");
  co_return;
}

class WebdavClient final : public RemoteFileClient {
public:
  WebdavEndpoint endpoint;
  std::shared_ptr<CurlHttpSession> session;
  bool transferring = false;

  RemoteFileCapabilities capabilities() const noexcept override { return {}; }

  cardio::promise<std::pair<CurlHttpResult, std::string>> propfind_async(
      std::string path, int depth, cardio::cancellation cancellation) {
    auto url = webdav_resource_url(endpoint, path);
    for (unsigned redirects = 0;; ++redirects) {
      CurlHttpRequest request;
      request.url = url;
      request.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
          "<d:getcontentlength/><d:getlastmodified/></d:prop></d:propfind>";
      request.headers = {"Depth: " + std::to_string(depth), "Content-Type: application/xml; charset=utf-8"};
      auto result = co_await perform_http_request_async(session, std::move(request), cancellation);
      if (result.code != CURLE_OK) throw webdav_http_error(result);
      if (result.status != 301 && result.status != 302 && result.status != 307 && result.status != 308)
        co_return std::make_pair(std::move(result), std::move(url));
      if (redirects == 5 || !result.headers.contains("location"))
        throw std::runtime_error("WebDAV redirect limit or missing location");
      const auto &location = result.headers.at("location");
      path = webdav_reference_path(endpoint, url, location);
      url = webdav_resource_url(endpoint, path);
      if (location.ends_with('/') && !url.ends_with('/')) url += '/';
    }
  }

  cardio::promise<RemoteDirectorySnapshot> load_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    path = webdav_virtual_path(std::move(path));
    auto [result, url] = co_await propfind_async(path, 1, cancellation);
    if (result.status != 207) throw webdav_http_error(result);
    const auto canonical = webdav_reference_path(endpoint, url, url);
    const auto resources = parse_webdav_multistatus(result.body, endpoint, url);
    RemoteDirectorySnapshot snapshot{canonical, {}};
    bool collection = false;
    for (const auto &resource : resources) {
      if (resource.status < 200 || resource.status >= 300)
        throw std::runtime_error("WebDAV collection contains a failed resource (HTTP " + std::to_string(resource.status) + ")");
      const auto &attributes = resource.attributes;
      if (attributes.path == canonical) {
        collection = attributes.type == RemoteFileType::directory;
        continue;
      }
      const auto slash = attributes.path.find_last_of('/');
      const auto parent = slash == 0 ? "/" : attributes.path.substr(0, slash);
      if (parent != canonical) throw std::runtime_error("DAV Depth: 1 response contains a non-child resource");
      snapshot.entries.push_back(attributes);
    }
    if (!collection) throw std::runtime_error("WebDAV path is not a collection");
    co_return snapshot;
  }

  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(
      std::string path, cardio::cancellation cancellation) override {
    auto [result, url] = co_await propfind_async(webdav_virtual_path(std::move(path)), 0, cancellation);
    if (result.status == 404) co_return std::nullopt;
    if (result.status != 207) throw webdav_http_error(result);
    const auto canonical = webdav_reference_path(endpoint, url, url);
    const auto resources = parse_webdav_multistatus(result.body, endpoint, url);
    if (resources.size() != 1 || resources.front().attributes.path != canonical)
      throw std::runtime_error("DAV Depth: 0 response does not identify the requested resource");
    const auto &resource = resources.front();
    if (resource.status == 404) co_return std::nullopt;
    if (resource.status < 200 || resource.status >= 300)
      throw std::runtime_error("WebDAV resource properties could not be read");
    co_return resource.attributes;
  }

  cardio::promise<std::string> read_link_async(std::string, cardio::cancellation) override {
    throw std::runtime_error("WebDAV does not support symbolic links");
    co_return std::string();
  }
  cardio::promise<void> make_directory_async(std::string, std::optional<std::uint32_t>, cardio::cancellation) override {
    co_await unavailable();
  }
  cardio::promise<void> remove_file_async(std::string, cardio::cancellation) override {
    co_await unavailable();
  }
  cardio::promise<void> remove_directory_async(std::string, cardio::cancellation) override {
    co_await unavailable();
  }
  cardio::promise<void> rename_async(std::string, std::string, cardio::cancellation) override {
    co_await unavailable();
  }
  cardio::promise<void> make_symbolic_link_async(std::string, std::string, cardio::cancellation) override {
    throw std::runtime_error("WebDAV does not support symbolic links");
    co_return;
  }
  cardio::promise<void> set_attributes_async(std::string, RemoteFileAttributes, cardio::cancellation) override {
    throw std::runtime_error("WebDAV does not support changing POSIX metadata");
    co_return;
  }
  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(std::string path, cardio::cancellation cancellation) override {
    auto pending = open_webdav_reader_async(session, endpoint, std::move(path), cancellation);
    auto reader = std::move(co_await pending);
    co_return reader;
  }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(
      std::string, std::optional<std::uint32_t>, cardio::cancellation) override {
    co_await unavailable();
    co_return nullptr;
  }
  bool try_begin_transfer() override {
    if (transferring) return false;
    transferring = true;
    return true;
  }
  void end_transfer() override { transferring = false; }
};

cardio::promise<std::shared_ptr<RemoteFileClient>> open_webdav_client_async(
    WebdavClientOpenOptions options, cardio::cancellation cancellation) {
  auto client = std::make_shared<WebdavClient>();
  client->endpoint = webdav_endpoint(options.connection);
  const auto directory = options.connection.remote_directory;
  client->session = create_curl_http_session(std::move(options.connection), std::move(options.password));
  co_await client->load_directory_async(directory, cancellation);
  co_return client;
}

cardio::promise<void> stop_webdav_client_async(std::shared_ptr<RemoteFileClient> client) {
  auto dav = std::dynamic_pointer_cast<WebdavClient>(client);
  if (!dav) throw std::invalid_argument("Client is not a WebDAV session");
  co_await stop_curl_http_session_async(dav->session);
}

} // namespace elder_terms
