#pragma once

#include <array>

#include <elder-terms/settings/serial-settings.h>

#include "../../activity-indicator-id.h"

namespace elder_terms {

/**
 * Snapshot of serial modem line states.
 */
struct SerialLineSignals {
  /** Request to send line. */
  bool rts = false;
  /** Clear to send line. */
  bool cts = false;
  /** Data terminal ready line. */
  bool dtr = false;
  /** Data set ready line. */
  bool dsr = false;
  /** Carrier detect / data carrier detect line. */
  bool cd = false;
  /** Ring indicator line. */
  bool ri = false;
};

/**
 * Carrier detection state-machine result.
 */
enum class SerialCarrierEvent {
  /** No disconnect event was observed. */
  none,
  /** The selected carrier signal dropped after being observed high. */
  disconnected,
};

/**
 * Tracks the selected carrier signal and reports high-to-low drops.
 */
class SerialCarrierTracker {
private:
  SerialCarrierDetect carrier_detect = SerialCarrierDetect::cd;
  bool observed_high = false;

  bool selected_signal_is_high(SerialLineSignals signals) const;

public:
  /**
   * Creates a carrier tracker.
   *
   * @param carrier_detect Selected signal to track.
   */
  explicit SerialCarrierTracker(SerialCarrierDetect carrier_detect);

  /**
   * Updates the tracker with the next line-state snapshot.
   *
   * @param signals Current modem line states.
   * @returns Disconnect event when the selected signal dropped.
   */
  SerialCarrierEvent update(SerialLineSignals signals);
};

/**
 * Builds modem-line signals from a TIOCMGET status bit field.
 *
 * @param status Raw TIOCMGET status bits.
 * @returns Decoded modem-line snapshot.
 */
SerialLineSignals serial_line_signals_from_modem_status(int status);

/**
 * Lists serial modem-line indicator states from one signal snapshot.
 *
 * @param signals Current modem-line signals.
 * @returns Activity indicator states for all modem lines.
 */
std::array<ActivityIndicatorState, 6>
serial_line_indicator_states(SerialLineSignals signals);

/**
 * Reads serial modem line states from an open fd.
 *
 * @param fd Open serial device fd.
 * @returns Current modem line states.
 *
 * @throws std::system_error when TIOCMGET fails.
 */
SerialLineSignals read_serial_line_signals(int fd);

} // namespace elder_terms
