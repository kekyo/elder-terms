#include "webdav-client.h"
#include "curl-http-session.h"
#include "webdav-metadata.h"
#include "webdav-stream.h"

#include <algorithm>
#include <stdexcept>

namespace elder_terms {

struct WebdavConnection {
  WebdavEndpoint endpoint;
  std::shared_ptr<CurlHttpSession> session;
};

static cardio::promise<std::pair<CurlHttpResult, std::string>> propfind_webdav_async(
    std::shared_ptr<WebdavConnection> connection, std::string path, int depth,
    cardio::cancellation cancellation) {
  auto url = webdav_resource_url(connection->endpoint, path);
  for (unsigned redirects = 0;; ++redirects) {
    CurlHttpRequest request;
    request.url = url;
    request.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
        "<d:getcontentlength/><d:getlastmodified/></d:prop></d:propfind>";
    request.headers = {"Depth: " + std::to_string(depth), "Content-Type: application/xml; charset=utf-8"};
    auto result = co_await perform_http_request_async(connection->session, std::move(request), cancellation);
    if (result.code != CURLE_OK) throw webdav_http_error(result);
    if (result.status != 301 && result.status != 302 && result.status != 307 && result.status != 308)
      co_return std::make_pair(std::move(result), std::move(url));
    if (redirects == 5 || !result.headers.contains("location"))
      throw std::runtime_error("WebDAV redirect limit or missing location");
    const auto &location = result.headers.at("location");
    path = webdav_reference_path(connection->endpoint, url, location);
    url = webdav_resource_url(connection->endpoint, path);
    if (location.ends_with('/') && !url.ends_with('/')) url += '/';
  }
}

static cardio::promise<RemoteDirectorySnapshot> load_webdav_directory_async(
    std::shared_ptr<WebdavConnection> connection, std::string path,
    cardio::cancellation cancellation) {
  path = webdav_virtual_path(std::move(path));
  auto [result, url] = co_await propfind_webdav_async(connection, path, 1, cancellation);
  if (result.status != 207) throw webdav_http_error(result);
  const auto canonical = webdav_reference_path(connection->endpoint, url, url);
  const auto resources = parse_webdav_multistatus(result.body, connection->endpoint, url);
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

static cardio::promise<std::optional<RemoteFileAttributes>> inspect_webdav_async(
    std::shared_ptr<WebdavConnection> connection, std::string path,
    cardio::cancellation cancellation) {
  auto [result, url] = co_await propfind_webdav_async(
      connection, webdav_virtual_path(std::move(path)), 0, cancellation);
  if (result.status == 404) co_return std::nullopt;
  if (result.status != 207) throw webdav_http_error(result);
  const auto canonical = webdav_reference_path(connection->endpoint, url, url);
  const auto resources = parse_webdav_multistatus(result.body, connection->endpoint, url);
  if (resources.size() != 1 || resources.front().attributes.path != canonical)
    throw std::runtime_error("DAV Depth: 0 response does not identify the requested resource");
  const auto &resource = resources.front();
  if (resource.status == 404) co_return std::nullopt;
  if (resource.status < 200 || resource.status >= 300)
    throw std::runtime_error("WebDAV resource properties could not be read");
  co_return resource.attributes;
}

static std::string webdav_mutation_path(std::string path) {
  path = webdav_virtual_path(std::move(path));
  if (path == "/") throw std::invalid_argument("The WebDAV virtual root cannot be changed");
  return path;
}

static std::string webdav_collection_url(const WebdavEndpoint &endpoint, const std::string &path) {
  auto url = webdav_resource_url(endpoint, path);
  if (!url.ends_with('/')) url += '/';
  return url;
}

static cardio::promise<void> mutate_webdav_async(
    std::shared_ptr<WebdavConnection> connection, CurlHttpRequest request,
    std::string path, cardio::cancellation cancellation) {
  const auto method = request.method;
  const auto url = request.url;
  const auto result = co_await perform_http_request_async(connection->session, std::move(request), cancellation);
  if (result.code != CURLE_OK)
    throw std::runtime_error("WebDAV " + method + " result is uncertain for " + path + ": " + webdav_http_error(result).what());
  if (result.status >= 300 && result.status < 400)
    throw std::runtime_error("WebDAV " + method + " redirect was not followed; check the configured resource path: " + path);
  if (result.status == 207) {
    const auto resources = parse_webdav_multistatus(result.body, connection->endpoint, url);
    for (const auto &resource : resources) {
      if (resource.status < 200 || resource.status >= 300)
        throw std::runtime_error("WebDAV " + method + " partially failed for " + resource.attributes.path +
            " (HTTP " + std::to_string(resource.status) + ")");
    }
    // DELETE/MOVE multistatus reports failures; an empty or success-only body
    // does not replace the normal final status required by these operations.
    throw std::runtime_error("WebDAV " + method + " returned an unexpected multistatus for " + path);
  }
  const bool succeeded = method == "MKCOL" ? result.status == 201
      : method == "MOVE" ? result.status == 201 || result.status == 204
      : result.status == 200 || result.status == 204;
  if (!succeeded) throw webdav_http_error(result);
}

static cardio::promise<void> make_webdav_directory_async(
    std::shared_ptr<WebdavConnection> connection, std::string path,
    cardio::cancellation cancellation) {
  path = webdav_mutation_path(std::move(path));
  CurlHttpRequest request;
  request.method = "MKCOL";
  request.url = webdav_collection_url(connection->endpoint, path);
  co_await mutate_webdav_async(connection, std::move(request), std::move(path), cancellation);
}

static cardio::promise<void> remove_webdav_async(
    std::shared_ptr<WebdavConnection> connection, std::string path,
    bool collection, cardio::cancellation cancellation) {
  path = webdav_mutation_path(std::move(path));
  const auto attributes = co_await inspect_webdav_async(connection, path, cancellation);
  if (!attributes) co_return;
  if ((attributes->type == RemoteFileType::directory) != collection)
    throw std::runtime_error("WebDAV deletion requires an explicit file or collection operation");
  CurlHttpRequest request;
  request.method = "DELETE";
  request.url = collection ? webdav_collection_url(connection->endpoint, path)
      : webdav_resource_url(connection->endpoint, path);
  co_await mutate_webdav_async(connection, std::move(request), std::move(path), cancellation);
}

static cardio::promise<void> move_webdav_async(
    std::shared_ptr<WebdavConnection> connection, std::string source,
    std::string destination, bool upload, bool overwrite,
    cardio::cancellation cancellation) {
  source = webdav_mutation_path(std::move(source));
  destination = webdav_mutation_path(std::move(destination));
  if (source == destination || destination.starts_with(source + '/'))
    throw std::invalid_argument("WebDAV cannot move a resource into itself");
  const auto attributes = co_await inspect_webdav_async(connection, source, cancellation);
  if (!attributes) throw std::runtime_error("WebDAV MOVE source does not exist");
  const bool collection = attributes->type == RemoteFileType::directory;
  if (upload) {
    const auto target = co_await inspect_webdav_async(connection, destination, cancellation);
    if (attributes->type != RemoteFileType::regular || (target && target->type != RemoteFileType::regular))
      throw std::runtime_error("WebDAV upload commit cannot replace a collection with a file");
  }
  CurlHttpRequest request;
  request.method = "MOVE";
  request.url = collection ? webdav_collection_url(connection->endpoint, source)
      : webdav_resource_url(connection->endpoint, source);
  const auto target_url = collection ? webdav_collection_url(connection->endpoint, destination)
      : webdav_resource_url(connection->endpoint, destination);
  request.headers = {"Destination: " + target_url, overwrite ? "Overwrite: T" : "Overwrite: F"};
  co_await mutate_webdav_async(connection, std::move(request), std::move(destination), cancellation);
}

class WebdavClient final : public RemoteFileClient {
public:
  std::shared_ptr<WebdavConnection> connection;
  bool transferring = false;

  RemoteFileCapabilities capabilities() const noexcept override {
    return {.upload_commit = true, .recursive_directory_delete = true};
  }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    return load_webdav_directory_async(connection, std::move(path), cancellation);
  }
  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(
      std::string path, cardio::cancellation cancellation) override {
    return inspect_webdav_async(connection, std::move(path), cancellation);
  }
  cardio::promise<std::string> read_link_async(std::string, cardio::cancellation) override {
    throw std::runtime_error("WebDAV does not support symbolic links");
    co_return std::string();
  }
  cardio::promise<void> make_directory_async(
      std::string path, std::optional<std::uint32_t>, cardio::cancellation cancellation) override {
    return make_webdav_directory_async(connection, std::move(path), cancellation);
  }
  cardio::promise<void> remove_file_async(std::string path, cardio::cancellation cancellation) override {
    return remove_webdav_async(connection, std::move(path), false, cancellation);
  }
  cardio::promise<void> remove_directory_async(std::string, cardio::cancellation) override {
    // A preceding empty listing cannot protect a concurrently added child.
    throw std::runtime_error("WebDAV requires explicit collection tree deletion; empty-only deletion is unavailable");
    co_return;
  }
  cardio::promise<void> remove_directory_tree_async(std::string path, cardio::cancellation cancellation) override {
    return remove_webdav_async(connection, std::move(path), true, cancellation);
  }
  cardio::promise<void> rename_async(std::string source, std::string destination, cardio::cancellation cancellation) override {
    return move_webdav_async(connection, std::move(source), std::move(destination), false, false, cancellation);
  }
  cardio::promise<void> commit_upload_async(std::string source, std::string destination, bool overwrite,
                                           cardio::cancellation cancellation) override {
    return move_webdav_async(connection, std::move(source), std::move(destination), true, overwrite, cancellation);
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
    return open_webdav_reader_async(connection->session, connection->endpoint, std::move(path), cancellation);
  }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(
      std::string path, std::uint64_t expected_size, std::optional<std::uint32_t>,
      cardio::cancellation cancellation) override {
    return open_webdav_writer_async(connection->session, connection->endpoint, std::move(path), expected_size, cancellation);
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
  client->connection = std::make_shared<WebdavConnection>();
  client->connection->endpoint = webdav_endpoint(options.connection);
  const auto directory = options.connection.remote_directory;
  client->connection->session = create_curl_http_session(std::move(options.connection), std::move(options.password));
  co_await client->load_directory_async(directory, cancellation);
  co_return client;
}

cardio::promise<void> stop_webdav_client_async(std::shared_ptr<RemoteFileClient> client) {
  auto dav = std::dynamic_pointer_cast<WebdavClient>(client);
  if (!dav) throw std::invalid_argument("Client is not a WebDAV session");
  co_await stop_curl_http_session_async(dav->connection->session);
}

} // namespace elder_terms
