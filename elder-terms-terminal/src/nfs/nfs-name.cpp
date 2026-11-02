// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-name.h"

#include <iconv.h>
#include <cerrno>
#include <array>
#include <stdexcept>
#include <glib.h>

namespace elder_terms {

struct NfsConversion {
  iconv_t descriptor;
  ~NfsConversion() { if (descriptor != reinterpret_cast<iconv_t>(-1)) iconv_close(descriptor); }
};

void validate_nfs_filename_encoding(const std::string &encoding) {
  if (encoding.empty() || encoding.find('/') != std::string::npos || encoding.find('\0') != std::string::npos)
    throw std::runtime_error("Filename encoding must not use replacement or ignore modifiers");
  NfsConversion decoder{iconv_open("UTF-8", encoding.c_str())};
  NfsConversion encoder{iconv_open(encoding.c_str(), "UTF-8")};
  if (decoder.descriptor == reinterpret_cast<iconv_t>(-1) || encoder.descriptor == reinterpret_cast<iconv_t>(-1))
    throw std::runtime_error("Unavailable filename encoding: " + encoding);
}

static std::string convert_name(std::string_view input, const char *from, const char *to) {
  NfsConversion conversion{iconv_open(to, from)};
  if (conversion.descriptor == reinterpret_cast<iconv_t>(-1))
    throw std::runtime_error("Unavailable filename encoding");
  char *cursor = const_cast<char *>(input.data());
  std::size_t remaining = input.size();
  std::string result;
  bool flushing = false;
  for (;;) {
    std::array<char, 512> buffer{};
    char *output = buffer.data();
    std::size_t available = buffer.size();
    errno = 0;
    const auto count = iconv(conversion.descriptor, flushing ? nullptr : &cursor,
        flushing ? nullptr : &remaining, &output, &available);
    result.append(buffer.data(), buffer.size() - available);
    if (count == static_cast<std::size_t>(-1)) {
      if (errno == E2BIG) continue;
      throw std::runtime_error("Filename cannot be converted without loss");
    }
    if (count != 0 || (!flushing && remaining != 0))
      throw std::runtime_error("Filename conversion is not reversible");
    if (flushing) break;
    flushing = true;
  }
  return result;
}

static std::string hex_bytes(std::string_view bytes, bool percent) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char byte : bytes) {
    if (percent) result += '%';
    else if (!result.empty()) result += ' ';
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

NfsDisplayName display_nfs_name(std::string_view bytes, const std::string &encoding) {
  validate_nfs_filename_encoding(encoding);
  try {
    const auto text = convert_name(bytes, encoding.c_str(), "UTF-8");
    if (text.find('\0') != std::string::npos || !g_utf8_validate(text.data(), text.size(), nullptr))
      throw std::runtime_error("Invalid display name");
    return {text, true};
  } catch (const std::runtime_error &) {
    return {"[undecodable: " + hex_bytes(bytes, false) + "]", false};
  }
}

std::string encode_nfs_name(std::string_view name, const std::string &encoding,
                           std::size_t maximum_bytes) {
  validate_nfs_filename_encoding(encoding);
  if (name.empty() || name.find('\0') != std::string_view::npos || name.find('/') != std::string_view::npos ||
      !g_utf8_validate(name.data(), name.size(), nullptr))
    throw std::runtime_error("Invalid UTF-8 filename component");
  const auto bytes = convert_name(name, "UTF-8", encoding.c_str());
  if (bytes.empty() || bytes == "." || bytes == ".." || bytes.size() > maximum_bytes ||
      bytes.find('\0') != std::string::npos || bytes.find('/') != std::string::npos)
    throw std::runtime_error("Encoded filename exceeds the NFS component constraints");
  if (convert_name(bytes, encoding.c_str(), "UTF-8") != name)
    throw std::runtime_error("Filename does not round-trip in " + encoding);
  return bytes;
}

std::string nfs_identifier_component(std::string_view bytes) { return hex_bytes(bytes, true); }

std::string nfs_identifier_bytes(std::string_view identifier) {
  if (identifier.empty() || identifier.size() % 3 != 0)
    throw std::runtime_error("Invalid NFS identifier");
  const auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("Noncanonical NFS identifier");
  };
  std::string result;
  for (std::size_t offset = 0; offset < identifier.size(); offset += 3) {
    if (identifier[offset] != '%') throw std::runtime_error("Invalid NFS identifier");
    result += static_cast<char>((nibble(identifier[offset + 1]) << 4) | nibble(identifier[offset + 2]));
  }
  return result;
}

} // namespace elder_terms
