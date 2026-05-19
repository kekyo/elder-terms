#pragma once

#include <termios.h>

#include <glib.h>

#include <elder-terms/settings/serial-settings.h>

namespace elder_terms {

/**
 * Platform termios flags derived from serial settings.
 */
struct SerialTermiosConfiguration {
  /** Control flags to clear before setting new values. */
  tcflag_t control_clear = 0;
  /** Control flags to set after clearing old values. */
  tcflag_t control_set = 0;
  /** Input flags to clear before setting new values. */
  tcflag_t input_clear = 0;
  /** Input flags to set after clearing old values. */
  tcflag_t input_set = 0;
  /** Standard termios speed when available. */
  speed_t standard_speed = B0;
  /** True when Linux termios2 BOTHER must be used. */
  bool custom_baudrate = false;
  /** Requested baud rate. */
  gint64 baudrate = 0;
};

/**
 * Builds the platform termios configuration for serial settings.
 *
 * @param settings Serial settings.
 * @returns Termios flags and baudrate mode.
 */
SerialTermiosConfiguration
serial_termios_configuration(SerialConnectionSettings settings);

/**
 * Applies serial settings to an open serial fd.
 *
 * @param fd Open serial device fd.
 * @param settings Serial settings.
 *
 * @throws std::system_error or std::runtime_error when configuration fails.
 */
void configure_serial_port(int fd, SerialConnectionSettings settings);

} // namespace elder_terms
