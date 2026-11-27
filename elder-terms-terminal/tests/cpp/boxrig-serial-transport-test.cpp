// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-transports.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

using namespace elder_terms;

// PTYs have real termios and byte I/O, but no modem wires. Supply only the
// modem snapshot at the OS boundary; all asynchronous waits remain real.
static int modem_status = TIOCM_RTS | TIOCM_CTS | TIOCM_CAR;
static int termios_updates = 0;
extern "C" int __real_ioctl(int descriptor, unsigned long request, ...);
extern "C" int __wrap_ioctl(int descriptor, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  auto *value = va_arg(arguments, void *);
  va_end(arguments);
  if (request == TIOCMGET) {
    *static_cast<int *>(value) = modem_status;
    return 0;
  }
  return __real_ioctl(descriptor, request, value);
}
extern "C" int __real_tcsetattr(int descriptor, int action, const termios *attributes);
extern "C" int __wrap_tcsetattr(int descriptor, int action, const termios *attributes) {
  ++termios_updates;
  return __real_tcsetattr(descriptor, action, attributes);
}

struct Fixture {
  int master = -1;
  std::optional<TerminalTransport> channel;
  std::optional<cardio::promise<void>> monitoring;
  cardio::cancellation_source stop;
  cardio::primitives::conditional changed;
  std::vector<ActivityIndicatorState> indicators;
  std::vector<bool> carrier;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void expect_port(Fixture *fixture, speed_t speed, bool software_flow) {
  termios attributes{};
  expect(tcgetattr(fixture->master, &attributes) == 0, "read applied port settings");
  expect(cfgetospeed(&attributes) == speed && cfgetispeed(&attributes) == speed,
      "both port speeds reflect the applied settings");
  expect((attributes.c_iflag & (IXON | IXOFF)) == (software_flow ? (IXON | IXOFF) : 0),
      "software flow control reflects the applied settings");
}

static cardio::promise<void> scenarios(Fixture *fixture, bool signals, bool handoff) {
  fixture->master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
  expect(fixture->master >= 0 && grantpt(fixture->master) == 0 && unlockpt(fixture->master) == 0, "create serial peer");
  const auto *path = ptsname(fixture->master);
  expect(path, "resolve serial peer");
  SerialConnectionSettings settings{
      .device = path, .device_match_mode = SerialDeviceMatchMode::exact_path, .device_usb_serial = {},
      .baudrate = 9600, .bits = 8, .parity = SerialParity::none, .stop_bit = 1,
      .flow_control = SerialFlowControl::none, .carrier_detect = SerialCarrierDetect::ignore};
  TerminalSessionCallbacks callbacks;
  callbacks.indicator_state = [fixture](ActivityIndicatorId id, bool active) {
    fixture->indicators.push_back({id, active});
    fixture->changed.trigger();
  };
  auto request = make_serial_transport_request(settings, "UTF-8", callbacks);
  auto recovery = request;
  if (!signals) expect(static_cast<bool>(request.configure_serial), "serial settings can change without reopening the channel");
  auto opening = request.open(24, 80, {});
  if (!signals) {
    settings.baudrate = 19200;
    request.configure_serial(settings);
  }
  fixture->channel = co_await opening;
  if (handoff) {
    const auto port = fixture->channel->duplicate_serial_port();
    expect(port >= 0, "retain the open serial device");
    co_await fixture->channel->close();
    request = make_serial_transport_request(settings, "CP932", callbacks);
    recovery = request;
    const auto updates = termios_updates;
    fixture->channel = request.adopt_serial_port(port);
    expect(termios_updates == updates, "adoption leaves line settings and queued port data intact");
    co_await fixture->channel->send_break({});
  }
  if (signals) {
    fixture->monitoring.emplace(fixture->channel->watch_carrier([fixture](bool connected) {
      fixture->carrier.push_back(connected);
      fixture->changed.trigger();
    }, fixture->stop.get_cancellation()));
    expect(fixture->indicators.size() == 6, "ignoring carrier still publishes modem-line indicators");
    expect(fixture->indicators[0].active && fixture->indicators[1].active && fixture->indicators[4].active,
        "RTS, CTS and CD show their actual high state when carrier is ignored");
    expect(!fixture->monitoring->is_ready(), "ignored carrier keeps observing signal changes");
    for (const auto selected : {SerialCarrierDetect::cts, SerialCarrierDetect::dsr, SerialCarrierDetect::ignore}) {
      const auto before = fixture->indicators.size();
      modem_status = TIOCM_DSR;
      settings.carrier_detect = selected;
      request.configure_serial(settings);
      while (fixture->indicators.size() == before) co_await fixture->changed.wait();
      expect(fixture->indicators.size() == before + 6 && fixture->indicators[before + 3].active &&
          !fixture->indicators[before + 1].active && !fixture->indicators[before + 4].active,
          "changing the selector refreshes indicators while the previous poll is pending");
      expect(fixture->carrier.back(), "a new carrier selector begins a fresh high-to-low observation");
      expect(!fixture->monitoring->is_ready(), "changing carrier keeps the monitor attached");
    }
    expect(termios_updates == 1, "carrier-only settings do not reconfigure the port");
    co_return;
  }
  expect_port(fixture, B19200, false);
  settings.baudrate = 115200;
  settings.flow_control = SerialFlowControl::xon;
  settings.device = "/does-not-exist/other-serial-target";
  settings.device_match_mode = SerialDeviceMatchMode::stable_id;
  settings.device_usb_serial = "different identity";
  recovery.configure_serial(settings);
  expect_port(fixture, B115200, true);
  const auto updates = termios_updates;
  request.configure_serial(settings);
  expect(termios_updates == updates, "unchanged live settings do not repeat a termios update");
  const std::array<unsigned char, 3> received{'r', 'x', '!'};
  expect(::write(fixture->master, received.data(), received.size()) == 3, "send after settings change");
  std::array<unsigned char, 3> bytes{};
  expect((co_await fixture->channel->read(bytes, {})) == bytes.size() && bytes == received,
      "the same open channel continues receiving");
  co_await fixture->channel->write(bytes, {});
  co_await cardio::from_fd(fixture->master, cardio::fd_event::read);
  bytes = {};
  expect(::read(fixture->master, bytes.data(), bytes.size()) == 3 && bytes == received,
      "the same open channel continues sending");
  co_await fixture->channel->close();
  fixture->channel.reset();
  settings.baudrate = 38400;
  request.configure_serial(settings);
  fixture->channel = co_await recovery.open(24, 80, {});
  expect_port(fixture, B38400, true);
  expect(::write(fixture->master, received.data(), received.size()) == 3, "send to recovered original identity");
  expect((co_await fixture->channel->read(bytes, {})) == bytes.size() && bytes == received,
      "recovery preserves the original device identity and the latest communication settings");
}

static cardio::promise<void> run(Fixture *fixture, cardio::dispatcher_group_glib *group,
    bool signals, bool handoff, std::exception_ptr *failure) {
  try { co_await scenarios(fixture, signals, handoff); }
  catch (...) { *failure = std::current_exception(); }
  fixture->stop.cancel();
  if (fixture->monitoring) {
    try { co_await *fixture->monitoring; }
    catch (const cardio::canceled_exception &) {}
    catch (...) { if (!*failure) *failure = std::current_exception(); }
  }
  if (fixture->channel) co_await fixture->channel->close();
  if (fixture->master >= 0) ::close(fixture->master);
  group->shutdown();
}

int main(int argc, char **argv) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  Fixture fixture;
  std::exception_ptr failure;
  const std::string mode = argc > 1 ? argv[1] : "";
  auto completion = run(&fixture, &group, mode == "signals", mode == "handoff", &failure);
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); }
    return 1;
  }
  return 0;
}
