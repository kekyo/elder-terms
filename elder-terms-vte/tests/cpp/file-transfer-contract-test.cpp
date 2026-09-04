#include "../../src/file-transfer/remote-file-client.h"

#include <iostream>
#include <optional>
#include <string>

namespace {

auto expect(bool condition, const char *message) -> bool {
  if (!condition) {
    std::cerr << "file-transfer-contract-test: FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

auto main() -> int {
  const elder_terms::RemoteFileCapabilities sftp{
      .symbolic_links = true,
      .permissions = true,
      .access_time = true,
      .modification_time = true,
  };
  const elder_terms::RemoteFileCapabilities ftp{
      .symbolic_links = false,
      .permissions = false,
      .access_time = false,
      .modification_time = true,
  };
  const elder_terms::RemoteFileAttributes entry{
      .name = "payload.bin",
      .path = "/remote/payload.bin",
      .type = elder_terms::RemoteFileType::regular,
      .size = 42,
      .permissions = std::nullopt,
      .access_time_unix_seconds = std::nullopt,
      .modification_time_unix_seconds = 1'725'177'600,
  };

  if (!expect(sftp.symbolic_links && sftp.permissions && sftp.access_time &&
                  sftp.modification_time,
              "SFTP capabilities should expose portable metadata") ||
      !expect(!ftp.symbolic_links && !ftp.permissions && !ftp.access_time &&
                  ftp.modification_time,
              "FTP capabilities should describe unsupported metadata") ||
      !expect(entry.type == elder_terms::RemoteFileType::regular &&
                  entry.size == 42 && !entry.permissions.has_value() &&
                  !entry.access_time_unix_seconds.has_value() &&
                  entry.modification_time_unix_seconds == 1'725'177'600,
              "remote entries should preserve optional metadata")) {
    return 1;
  }

  std::cout << "file-transfer-contract-test: PASS\n";
  return 0;
}
