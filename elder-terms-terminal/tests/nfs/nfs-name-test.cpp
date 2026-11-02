// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/nfs/nfs-name.h"
#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <vector>

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static void rejects(const std::function<void()> &operation) {
  bool rejected = false;
  try { operation(); } catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "Invalid or lossy name must fail before a request is issued");
}
int main() {
  using namespace elder_terms;
  try {
    const std::string cp932 = "\x93\xfa\x96\x7b\x8c\xea\x95\x5c";
    const std::string eucjp = "\xc6\xfc\xcb\xdc\xb8\xec\xc9\xbd";
    for (const auto &[encoding, bytes] : std::vector<std::pair<std::string, std::string>>{
        {"UTF-8", "日本語表"}, {"CP932", cp932}, {"SHIFT-JIS", cp932}, {"EUC-JP", eucjp}}) {
      validate_nfs_filename_encoding(encoding);
      const auto displayed = display_nfs_name(bytes, encoding);
      require(displayed.decoded && displayed.text == "日本語表", "Server bytes must decode using the explicit encoding");
      require(encode_nfs_name(displayed.text, encoding, 255) == bytes, "New names must round-trip exactly");
    }
    for (const std::string encoding : {"UTF-8", "CP932", "EUC-JP"}) {
      const auto invalid = display_nfs_name(std::string("\xff", 1), encoding);
      require(!invalid.decoded && g_utf8_validate(invalid.text.c_str(), -1, nullptr) &&
          invalid.text.find("FF") != std::string::npos, "Invalid names must retain a valid UTF-8 hex presentation");
    }
    for (const std::string name : {" ", "?", "#", "%", "\\", "が", "が"})
      require(encode_nfs_name(name, "UTF-8", 255) == name, "Names must not be trimmed, normalized, or URL-parsed");
    for (const auto &name : std::vector<std::string>{"", ".", "..", "a/b", std::string("a\0b", 3), "\xff"})
      rejects([&] { (void)encode_nfs_name(name, "UTF-8", 255); });
    rejects([] { (void)encode_nfs_name("😀", "CP932", 255); });
    rejects([] { (void)encode_nfs_name("a", "UTF-16LE", 255); });
    rejects([] { (void)encode_nfs_name("日本語表", "CP932", 7); });
    rejects([] { (void)encode_nfs_name("¥", "CP932", 255); });
    for (const std::string encoding : {"UTF-8//IGNORE", "CP932//TRANSLIT", "unknown-encoding", ""})
      rejects([&] { validate_nfs_filename_encoding(encoding); });
    for (const auto &bytes : std::vector<std::string>{"/", "/srv/public", "?foo#%20", cp932, "\xff", " a "}) {
      const auto id = nfs_identifier_component(bytes);
      require(id.find('/') == std::string::npos && id.find('?') == std::string::npos &&
          id.find('#') == std::string::npos && g_utf8_validate(id.c_str(), -1, nullptr), "Identifier must be one UTF-8-safe path component");
      require(nfs_identifier_bytes(id) == bytes, "Identifier decoding must preserve every byte");
    }
    require(nfs_identifier_component("/srv/public") != nfs_identifier_component("/other/public"), "Equal basenames must have distinct roots");
    for (const std::string invalid : {"%", "%ff", "%GG", "plain"})
      rejects([&] { (void)nfs_identifier_bytes(invalid); });
    std::cout << "PASS NFS name encoding and byte identity\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
