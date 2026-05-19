#pragma once

#include <elder-terms/settings/serial-settings.h>

namespace elder_terms {

/**
 * Snapshot of serial modem line states.
 */
struct SerialLineSignals {
  /** Carrier detect / data carrier detect line. */
  bool cd = false;
  /** Clear to send line. */
  bool cts = false;
  /** Data set ready line. */
  bool dsr = false;
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
 * Reads serial modem line states from an open fd.
 *
 * @param fd Open serial device fd.
 * @returns Current modem line states.
 *
 * @throws std::system_error when TIOCMGET fails.
 */
SerialLineSignals read_serial_line_signals(int fd);

} // namespace elder_terms
