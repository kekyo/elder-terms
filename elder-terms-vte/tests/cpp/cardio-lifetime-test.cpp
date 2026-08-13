#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>

#include <cardio.h>

static bool timeout_cancellation_survives_dispatcher_destruction() {
  using Dispatcher = cardio::dispatcher_host_glib;

  cardio::dispatcher_group_glib group;
  alignas(Dispatcher) std::array<std::byte, sizeof(Dispatcher)> storage{};
  auto timeout = std::optional<cardio::cancellation_source>{};

  auto *dispatcher =
      std::construct_at(reinterpret_cast<Dispatcher *>(storage.data()), group);
  timeout.emplace(cardio::cancellations::timeout(60000));
  std::destroy_at(dispatcher);
  storage.fill(std::byte{0xa5});

  return timeout->cancel() &&
         timeout->get_cancellation().is_cancellation_requested();
}

int main() {
  if (!timeout_cancellation_survives_dispatcher_destruction()) {
    std::cerr << "timeout cancellation did not survive dispatcher destruction\n";
    return 1;
  }

  return 0;
}
