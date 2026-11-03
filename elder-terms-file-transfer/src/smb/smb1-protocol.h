// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <array>
#include <cardio.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace elder_terms::smb1 {

// A private, bounded protocol between two executables from the same build.
// Credentials travel only in the first frame, never in process arguments.
inline constexpr std::size_t maximum_frame = 16 * 1024 * 1024;
inline constexpr std::size_t chunk_size = 32768;
enum class Command : std::uint32_t {
  connect = 1, shares, list, stat, mkdir, remove, rmdir, rename,
  read_open, write_open, read, write, close, commit, error = 0xffffffff,
};

struct Payload {
  std::vector<std::byte> bytes;
  void number(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes.push_back(static_cast<std::byte>((value >> shift) & 255));
  }
  void data(std::span<const std::byte> value) {
    if (value.size() > maximum_frame || bytes.size() + value.size() + 8 > maximum_frame)
      throw std::runtime_error("SMB1 helper message exceeds its size limit");
    number(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  void text(const std::string &value) {
    data(std::as_bytes(std::span(value.data(), value.size())));
  }
};

struct Cursor {
  std::span<const std::byte> bytes;
  std::uint64_t number() {
    if (bytes.size() < 8) throw std::runtime_error("Truncated SMB1 helper response");
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
      value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    bytes = bytes.subspan(8);
    return value;
  }
  std::span<const std::byte> data() {
    const auto size = number();
    if (size > bytes.size()) throw std::runtime_error("Invalid SMB1 helper payload size");
    const auto result = bytes.first(static_cast<std::size_t>(size));
    bytes = bytes.subspan(result.size());
    return result;
  }
  std::string text() {
    const auto value = data();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
  }
  void finish() const {
    if (!bytes.empty()) throw std::runtime_error("Unexpected SMB1 helper payload");
  }
};

struct Frame {
  std::uint64_t id;
  Command command;
  Payload payload;
};

inline cardio::promise<void> send_frame(GOutputStream *stream, const Frame &frame,
                                        cardio::cancellation cancellation) {
  if (frame.payload.bytes.size() > maximum_frame)
    throw std::runtime_error("SMB1 helper message exceeds its size limit");
  Payload header;
  header.number(frame.id);
  header.number(static_cast<std::uint32_t>(frame.command));
  header.number(frame.payload.bytes.size());
  header.bytes.insert(header.bytes.end(), frame.payload.bytes.begin(), frame.payload.bytes.end());
  auto remaining = std::span<const std::byte>(header.bytes);
  while (!remaining.empty()) {
    const auto count = co_await cardio::gio::write(stream, remaining, cancellation);
    if (count == 0) throw std::runtime_error("SMB1 helper write made no progress");
    remaining = remaining.subspan(count);
  }
}

inline cardio::promise<Frame> receive_frame(GInputStream *stream, cardio::cancellation cancellation) {
  std::array<std::byte, 24> header{};
  if (co_await cardio::gio::read_all(stream, header, cancellation) != header.size())
    throw std::runtime_error("SMB1 helper connection closed");
  Cursor cursor{header};
  Frame result{cursor.number(), static_cast<Command>(cursor.number()), {}};
  const auto size = cursor.number();
  if (size > maximum_frame) throw std::runtime_error("SMB1 helper message exceeds its size limit");
  result.payload.bytes.resize(static_cast<std::size_t>(size));
  if (size != 0 && co_await cardio::gio::read_all(stream,
      std::span<std::byte>(result.payload.bytes), cancellation) != size)
    throw std::runtime_error("SMB1 helper response ended early");
  co_return result;
}

} // namespace elder_terms::smb1
