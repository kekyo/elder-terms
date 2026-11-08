// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb1-protocol.h"
#include <algorithm>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  using namespace elder_terms::smb1;
  try {
    for (const auto size : {std::size_t{0}, std::size_t{1}, chunk_size, maximum_frame - 16}) {
      std::vector<std::byte> input(size);
      for (std::size_t index = 0; index < size; ++index)
        input[index] = static_cast<std::byte>(index & 255);
      Payload payload;
      payload.number(0x123456789abcdef0);
      payload.data(input);
      require(payload.bytes.size() == size + 16, "Payload must contain the prefix, length and data");
      Cursor cursor{payload.bytes};
      require(cursor.number() == 0x123456789abcdef0, "Appending data changed the existing prefix");
      const auto decoded = cursor.data();
      require(decoded.size() == size && std::equal(input.begin(), input.end(), decoded.begin()),
          "Payload data did not round-trip");
      cursor.finish();
    }
    Payload empty;
    empty.data({});
    Cursor empty_cursor{empty.bytes};
    require(empty_cursor.data().empty(), "An empty span must encode an empty field");
    empty_cursor.finish();

    Payload full;
    full.number(1);
    const auto original = full.bytes;
    const std::vector<std::byte> oversized(maximum_frame - 15);
    bool rejected = false;
    try { full.data(oversized); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected && full.bytes == original, "An oversized field must be rejected before changing the payload");
    std::cout << "PASS SMB1 payload boundaries and byte identity\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
