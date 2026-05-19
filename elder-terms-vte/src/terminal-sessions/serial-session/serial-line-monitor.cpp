#include "serial-line-monitor.h"

#include <sys/ioctl.h>

#include <cerrno>
#include <system_error>

namespace elder_terms {

bool SerialCarrierTracker::selected_signal_is_high(
    SerialLineSignals signals) const {
  if (carrier_detect == SerialCarrierDetect::cts) {
    return signals.cts;
  }
  if (carrier_detect == SerialCarrierDetect::dsr) {
    return signals.dsr;
  }
  return signals.cd;
}

SerialCarrierTracker::SerialCarrierTracker(
    SerialCarrierDetect carrier_detect)
    : carrier_detect(carrier_detect) {
}

SerialCarrierEvent SerialCarrierTracker::update(SerialLineSignals signals) {
  const bool high = selected_signal_is_high(signals);
  if (high) {
    observed_high = true;
    return SerialCarrierEvent::none;
  }

  if (observed_high) {
    observed_high = false;
    return SerialCarrierEvent::disconnected;
  }
  return SerialCarrierEvent::none;
}

SerialLineSignals read_serial_line_signals(int fd) {
  int status = 0;
  if (::ioctl(fd, TIOCMGET, &status) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "serial TIOCMGET failed");
  }

  return {
      .cd = (status & TIOCM_CAR) != 0,
      .cts = (status & TIOCM_CTS) != 0,
      .dsr = (status & TIOCM_DSR) != 0,
  };
}

} // namespace elder_terms
