#include "file-hash.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include <gio/gio.h>
#include <glib.h>

namespace elder_terms {

static constexpr std::size_t file_hash_read_size = 64 * 1024;

struct HashGObjectDeleter {
  void operator()(void *object) const {
    if (object != nullptr) {
      g_object_unref(object);
    }
  }
};

struct GChecksumDeleter {
  void operator()(GChecksum *checksum) const {
    if (checksum != nullptr) {
      g_checksum_free(checksum);
    }
  }
};

template <typename T>
using HashGObjectPtr = std::unique_ptr<T, HashGObjectDeleter>;
using GChecksumPtr = std::unique_ptr<GChecksum, GChecksumDeleter>;

static GChecksumPtr create_checksum(GChecksumType type) {
  GChecksumPtr checksum(g_checksum_new(type));
  if (checksum == nullptr) {
    throw std::runtime_error("Failed to allocate file checksum");
  }
  return checksum;
}

static std::string checksum_string(const GChecksumPtr &checksum) {
  const char *value = g_checksum_get_string(checksum.get());
  if (value == nullptr) {
    throw std::runtime_error("Failed to finalize file checksum");
  }
  return value;
}

cardio::promise<FileHashes>
calculate_local_file_hashes_async(std::string path,
                                  cardio::cancellation cancellation) {
  if (path.empty()) {
    throw std::invalid_argument("Local file path is required");
  }

  HashGObjectPtr<GFile> file(g_file_new_for_path(path.c_str()));
  HashGObjectPtr<GInputStream> stream(G_INPUT_STREAM(
      co_await cardio::gio::read(file.get(), G_PRIORITY_DEFAULT)));
  GChecksumPtr md5 = create_checksum(G_CHECKSUM_MD5);
  GChecksumPtr sha1 = create_checksum(G_CHECKSUM_SHA1);
  GChecksumPtr sha256 = create_checksum(G_CHECKSUM_SHA256);
  std::array<std::byte, file_hash_read_size> buffer{};
  for (;;) {
    const std::size_t read_size = co_await cardio::gio::read(
        stream.get(), std::span<std::byte>(buffer.data(), buffer.size()),
        cancellation, G_PRIORITY_DEFAULT);
    if (read_size == 0) {
      break;
    }
    cancellation.throw_if_cancellation_requested();
    const auto *bytes = reinterpret_cast<const guchar *>(buffer.data());
    g_checksum_update(md5.get(), bytes, read_size);
    g_checksum_update(sha1.get(), bytes, read_size);
    g_checksum_update(sha256.get(), bytes, read_size);
  }
  cancellation.throw_if_cancellation_requested();
  co_await cardio::gio::close(stream.get(), G_PRIORITY_DEFAULT);

  co_return FileHashes{
      .md5 = checksum_string(md5),
      .sha1 = checksum_string(sha1),
      .sha256 = checksum_string(sha256),
  };
}

} // namespace elder_terms
