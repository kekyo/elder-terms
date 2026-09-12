#include "webdav-metadata.h"

#include <libxml/xmlreader.h>
#include <curl/curl.h>
#include <charconv>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>

namespace elder_terms {

struct DavNode {
  std::string name;
  std::string text;
  std::vector<std::unique_ptr<DavNode>> children;
};

static std::string xml_string(const xmlChar *value) {
  return value ? reinterpret_cast<const char *>(value) : "";
}

static void ignore_xml_error(void *, const char *, xmlParserSeverities, xmlTextReaderLocatorPtr) {
  // Parser diagnostics can contain server-controlled text; callers report a
  // bounded, protocol-specific error without exposing it to terminal output.
}

static std::unique_ptr<DavNode> read_document(const std::string &xml) {
  if (xml.empty() || xml.size() > 32 * 1024 * 1024)
    throw std::runtime_error("Invalid WebDAV XML response size");
  const auto reader = std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)>(
      xmlReaderForMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                         XML_PARSE_NONET), xmlFreeTextReader);
  if (!reader) throw std::runtime_error("Could not create WebDAV XML reader");
  xmlTextReaderSetErrorHandler(reader.get(), ignore_xml_error, nullptr);
  // Explicitly disable all entity/DTD processing, including library defaults.
  // NONET alone would still allow external entities from the local filesystem.
  for (const int property : {XML_PARSER_LOADDTD, XML_PARSER_DEFAULTATTRS,
                             XML_PARSER_VALIDATE, XML_PARSER_SUBST_ENTITIES})
    if (xmlTextReaderSetParserProp(reader.get(), property, 0) != 0)
      throw std::runtime_error("Could not disable WebDAV XML external resources");
  std::unique_ptr<DavNode> root;
  std::vector<DavNode *> stack;
  std::size_t nodes = 0;
  int result = 0;
  while ((result = xmlTextReaderRead(reader.get())) == 1) {
    const auto type = xmlTextReaderNodeType(reader.get());
    if (type == XML_READER_TYPE_DOCUMENT_TYPE || type == XML_READER_TYPE_ENTITY_REFERENCE ||
        type == XML_READER_TYPE_ENTITY)
      throw std::runtime_error("WebDAV XML cannot contain DTDs or entity references");
    if (type == XML_READER_TYPE_ELEMENT) {
      if (++nodes > 200000 || stack.size() >= 64)
        throw std::runtime_error("WebDAV XML exceeds structural limits");
      const auto base = std::unique_ptr<xmlChar, decltype(xmlFree)>(
          xmlTextReaderGetAttributeNs(reader.get(), BAD_CAST "base", BAD_CAST "http://www.w3.org/XML/1998/namespace"), xmlFree);
      if (base) throw std::runtime_error("WebDAV XML base overrides are not supported");
      auto node = std::make_unique<DavNode>();
      const auto namespace_uri = xml_string(xmlTextReaderConstNamespaceUri(reader.get()));
      if (namespace_uri == "http://www.w3.org/2001/XInclude")
        throw std::runtime_error("WebDAV XML cannot contain XInclude references");
      if (namespace_uri == "DAV:")
        node->name = xml_string(xmlTextReaderConstLocalName(reader.get()));
      auto *current = node.get();
      if (stack.empty()) {
        if (root) throw std::runtime_error("Multiple WebDAV XML roots");
        root = std::move(node);
      } else stack.back()->children.push_back(std::move(node));
      if (xmlTextReaderIsEmptyElement(reader.get()) != 1) stack.push_back(current);
    } else if (type == XML_READER_TYPE_END_ELEMENT) {
      if (stack.empty()) throw std::runtime_error("Invalid WebDAV XML nesting");
      stack.pop_back();
    } else if (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
               type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE || type == XML_READER_TYPE_WHITESPACE) {
      if (!stack.empty()) {
        const auto value = xml_string(xmlTextReaderConstValue(reader.get()));
        if (value.size() > 64 * 1024 - stack.back()->text.size())
          throw std::runtime_error("WebDAV XML field exceeds its limit");
        stack.back()->text += value;
      }
    }
  }
  if (result < 0 || !root || !stack.empty() || root->name != "multistatus")
    throw std::runtime_error("Invalid WebDAV multistatus XML");
  return root;
}

static const DavNode *child(const DavNode &node, const char *name) {
  const DavNode *found = nullptr;
  for (const auto &item : node.children) if (item->name == name) {
    if (found) throw std::runtime_error("Duplicate WebDAV XML field");
    found = item.get();
  }
  return found;
}

static std::string_view trim(std::string_view text) {
  while (!text.empty() && g_ascii_isspace(text.front())) text.remove_prefix(1);
  while (!text.empty() && g_ascii_isspace(text.back())) text.remove_suffix(1);
  return text;
}

static int http_status(const DavNode &node) {
  auto line = trim(node.text);
  if (!node.children.empty() || !line.starts_with("HTTP/"))
    throw std::runtime_error("Invalid DAV status line");
  const auto space = line.find(' ');
  if (space == std::string_view::npos) throw std::runtime_error("Missing DAV status code");
  line.remove_prefix(space + 1);
  int status = 0;
  const auto parsed = std::from_chars(line.data(), line.data() + line.size(), status);
  if (parsed.ec != std::errc{} || parsed.ptr != line.data() + 3 || status < 100 || status > 599 ||
      (line.size() > 3 && line[3] != ' ')) throw std::runtime_error("Invalid DAV status code");
  return status;
}

std::vector<WebdavResource> parse_webdav_multistatus(
    const std::string &xml, const WebdavEndpoint &endpoint,
    const std::string &request_url) {
  const auto root = read_document(xml);
  std::vector<WebdavResource> resources;
  std::set<std::string> paths;
  for (const auto &response : root->children) {
    if (response->name != "response") continue;
    const auto *href = child(*response, "href");
    if (!href || !href->children.empty()) throw std::runtime_error("Missing DAV resource href");
    WebdavResource resource;
    auto &attributes = resource.attributes;
    attributes.path = webdav_reference_path(endpoint, request_url, std::string(trim(href->text)));
    attributes.name = attributes.path == "/" ? "/" : attributes.path.substr(attributes.path.find_last_of('/') + 1);
    if (!paths.insert(attributes.path).second) throw std::runtime_error("Duplicate DAV resource href");
    if (const auto *status = child(*response, "status")) resource.status = http_status(*status);
    std::map<std::string, const DavNode *> properties;
    bool has_propstat = false;
    int failed_status = 0;
    for (const auto &propstat : response->children) {
      if (propstat->name != "propstat") continue;
      has_propstat = true;
      const auto *status = child(*propstat, "status");
      const auto *prop = child(*propstat, "prop");
      if (!status || !prop) throw std::runtime_error("Incomplete DAV propstat");
      const auto code = http_status(*status);
      if (code < 200 || code >= 300) { failed_status = code; continue; }
      for (const auto &property : prop->children) {
        if (property->name.empty()) continue;
        if (!properties.emplace(property->name, property.get()).second)
          throw std::runtime_error("Duplicate successful DAV property");
      }
    }
    if (has_propstat && resource.status) throw std::runtime_error("Ambiguous DAV response status");
    if (has_propstat) resource.status = properties.empty() ? failed_status : 200;
    if (!resource.status) throw std::runtime_error("Missing DAV response status");
    if (properties.contains("resourcetype")) {
      const auto &type = *properties.at("resourcetype");
      attributes.type = child(type, "collection") ? RemoteFileType::directory
          : type.children.empty() ? RemoteFileType::regular : RemoteFileType::other;
    }
    if (properties.contains("getcontentlength")) {
      const auto &property = *properties.at("getcontentlength");
      const auto value = trim(property.text);
      std::uint64_t size = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
      if (!property.children.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("Invalid DAV content length");
      attributes.size = size;
    }
    if (properties.contains("getlastmodified")) {
      const auto &value = properties.at("getlastmodified")->text;
      const auto time = curl_getdate(value.c_str(), nullptr);
      if (time != -1) attributes.modification_time_unix_seconds = static_cast<std::int64_t>(time);
    }
    resources.push_back(std::move(resource));
  }
  return resources;
}

} // namespace elder_terms
