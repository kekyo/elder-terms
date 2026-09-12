#include "webdav-metadata.h"

#include <iostream>
#include <stdexcept>

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

template<typename Action> static void rejected(Action action, const char *message) {
  bool failed = false;
  try { action(); } catch (const std::exception &) { failed = true; }
  expect(failed, message);
}

int main() {
  try {
    WebdavConnectionSettings settings;
    settings.address = "EXAMPLE.test";
    settings.base_path = "/dav/root%20folder/";
    const auto endpoint = webdav_endpoint(settings);
    const auto url = webdav_resource_url(endpoint, "/");
    expect(url == "https://example.test:443/dav/root%20folder/", "Endpoint must preserve encoded base path");
    const auto special = webdav_resource_url(endpoint, "/資料 #+%.txt");
    expect(webdav_reference_path(endpoint, url, special) == "/資料 #+%.txt", "Unicode and reserved characters must round-trip once");
    expect(webdav_reference_path(endpoint, url, "literal%252F.txt") == "/literal%2F.txt", "Literal percent escapes must not decode twice");
    expect(webdav_reference_path(endpoint, url, "https://EXAMPLE.test/dav/root%20folder/a") == "/a", "Default ports and host case must identify the same origin");
    for (const auto *href : {"https://other.test/dav/root%20folder/a", "http://example.test/dav/root%20folder/a",
          "/dav/root%20folder2/a", "/dav/root%20folder/../a", "a%2fb", "a%5Cb", "%2e%2e/a",
          "a%00b", "a%0db", "a%ffb", "a?query", "a#fragment", "//other.test/a",
          "https://alice@example.test/dav/root%20folder/a"})
      rejected([&] { (void)webdav_reference_path(endpoint, url, href); }, "Unsafe href must be rejected");
    const std::string start = "<x:multistatus xmlns:x=\"DAV:\">";
    const std::string finish = "</x:multistatus>";
    const auto response = [](const std::string &href, const std::string &properties) {
      return "<x:response><x:href>" + href + "</x:href><x:propstat><x:prop>" + properties +
          "</x:prop><x:status>HTTP/1.1 200 OK</x:status></x:propstat></x:response>";
    };
    const auto parsed = parse_webdav_multistatus(start +
        response("/dav/root%20folder/", "<x:resourcetype><x:collection/></x:resourcetype>") +
        response("zero", "<x:resourcetype/><x:getcontentlength>0</x:getcontentlength>") +
        response("unknown", "<x:resourcetype/><x:displayname>ignored</x:displayname>") +
        response("large", "<x:resourcetype/><x:getcontentlength>4294967313</x:getcontentlength><x:getlastmodified>Wed, 01 Jan 2025 00:00:00 GMT</x:getlastmodified>") + finish, endpoint, url);
    expect(parsed.size() == 4 && parsed[0].attributes.type == RemoteFileType::directory, "Collection properties must parse by namespace URI");
    expect(parsed[1].attributes.size == 0 && !parsed[2].attributes.size, "Unknown size must differ from zero");
    expect(parsed[2].attributes.name == "unknown", "Displayname must not identify resources");
    expect(parsed[3].attributes.size == 4294967313ULL && parsed[3].attributes.modification_time_unix_seconds == 1735689600,
           "Large sizes and RFC date values must be preserved");
    const auto denied = parse_webdav_multistatus(start + "<x:response><x:href>denied</x:href><x:status>HTTP/1.1 403 Forbidden</x:status></x:response>" + finish, endpoint, url);
    expect(denied.size() == 1 && denied[0].status == 403, "207 must preserve failed resources");
    rejected([&] {
      (void)parse_webdav_multistatus(start + response("/dav/root%20folder/", "<x:resourcetype><x:collection/></x:resourcetype>") +
          "<xi:include xmlns:xi=\"http://www.w3.org/2001/XInclude\" href=\"file:///etc/passwd\" parse=\"text\"/>" + finish,
          endpoint, url);
    }, "XInclude must be rejected rather than silently omitting resources");
    for (const std::string &bad : std::vector<std::string>{
        std::string("<multistatus/>"), start,
        start + response("a", "<x:getcontentlength>-1</x:getcontentlength>") + finish,
        start + response("a", "<x:getcontentlength>18446744073709551616</x:getcontentlength>") + finish,
        start + response("a", "<x:resourcetype/>") + response("a", "<x:resourcetype/>") + finish,
        "<!DOCTYPE x:multistatus [<!ENTITY local SYSTEM \"file:///etc/passwd\">]>" + start + response("a", "<x:displayname>&local;</x:displayname>") + finish,
        "<!DOCTYPE x:multistatus SYSTEM \"http://127.0.0.1:9/no-network\">" + start + finish,
        "<x:multistatus xmlns:x=\"DAV:\" xml:base=\"https://other.test/\"/>"})
      rejected([&] { (void)parse_webdav_multistatus(bad, endpoint, url); }, "Malformed or unsafe XML must fail closed");
    std::cout << "webdav-metadata-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
