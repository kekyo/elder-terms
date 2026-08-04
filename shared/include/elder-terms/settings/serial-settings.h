#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glib.h>

#include <elder-terms/export.h>
#include <elder-terms/serial-device.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Serial parity configuration.
 */
enum class SerialParity {
  /** No parity bit. */
  none,
  /** Even parity. */
  even,
  /** Odd parity. */
  odd,
};

/**
 * Serial flow-control configuration.
 */
enum class SerialFlowControl {
  /** No software or hardware flow control. */
  none,
  /** XON/XOFF software flow control. */
  xon,
  /** RTS/CTS hardware flow control. */
  hard,
};

/**
 * Serial connection monitoring behavior.
 */
enum class SerialCarrierDetect {
  /** Keep the session active without monitoring modem-line state. */
  ignore,
  /** Carrier detect / data carrier detect line. */
  cd,
  /** Clear to send line. */
  cts,
  /** Data set ready line. */
  dsr,
};

/**
 * Settings for the serial connection backend.
 */
struct SerialConnectionSettings {
  /** Serial device path or identifier. */
  std::string device;
  /** Strategy used to identify the serial device after reattachment. */
  SerialDeviceMatchMode device_match_mode = SerialDeviceMatchMode::stable_id;
  /** Remembered USB serial number used as a supplemental stable-ID match. */
  std::optional<std::string> device_usb_serial;
  /** Serial baud rate. */
  gint64 baudrate;
  /** Data bits per character. */
  gint64 bits;
  /** Parity mode. */
  SerialParity parity;
  /** Stop bits per character. */
  gint64 stop_bit;
  /** Flow-control mode. */
  SerialFlowControl flow_control;
  /** Modem line used to detect disconnects, or ignore to disable monitoring. */
  SerialCarrierDetect carrier_detect;
};

/**
 * Returns serial setting definitions.
 *
 * @returns Setting definitions for the serial INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
serial_connection_setting_definitions();

/**
 * Returns the setting key for [serial] device.
 *
 * @returns Setting key for the serial device selector.
 */
ELDER_TERMS_API SettingKey serial_device_setting_key();

/**
 * Returns the setting key for [serial] device_match_mode.
 *
 * @returns Setting key for the serial device identification strategy.
 */
ELDER_TERMS_API SettingKey serial_device_match_mode_setting_key();

/**
 * Returns the setting key for [serial] device_usb_serial.
 *
 * @returns Setting key for the remembered USB serial number.
 */
ELDER_TERMS_API SettingKey serial_device_usb_serial_setting_key();

/**
 * Returns the setting key for [serial] baudrate.
 *
 * @returns Setting key for the serial baud rate.
 */
ELDER_TERMS_API SettingKey serial_baudrate_setting_key();

/**
 * Returns the setting key for [serial] bits.
 *
 * @returns Setting key for serial data bits.
 */
ELDER_TERMS_API SettingKey serial_bits_setting_key();

/**
 * Returns the setting key for [serial] parity.
 *
 * @returns Setting key for serial parity.
 */
ELDER_TERMS_API SettingKey serial_parity_setting_key();

/**
 * Returns the setting key for [serial] stop_bit.
 *
 * @returns Setting key for serial stop bits.
 */
ELDER_TERMS_API SettingKey serial_stop_bit_setting_key();

/**
 * Returns the setting key for [serial] flow_control.
 *
 * @returns Setting key for serial flow control.
 */
ELDER_TERMS_API SettingKey serial_flow_control_setting_key();

/**
 * Returns the setting key for [serial] carrier_detect.
 *
 * @returns Setting key for serial carrier detection.
 */
ELDER_TERMS_API SettingKey serial_carrier_detect_setting_key();

/**
 * Extracts serial connection settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed serial connection settings.
 */
ELDER_TERMS_API SerialConnectionSettings
serial_connection_settings(const SettingsStore &store);

/**
 * Converts a serial parity enum to its INI value.
 *
 * @param parity Serial parity.
 * @returns INI value for the parity.
 */
ELDER_TERMS_API std::string serial_parity_to_string(SerialParity parity);

/**
 * Converts a serial flow-control enum to its INI value.
 *
 * @param flow_control Serial flow-control mode.
 * @returns INI value for the flow-control mode.
 */
ELDER_TERMS_API std::string
serial_flow_control_to_string(SerialFlowControl flow_control);

/**
 * Converts a serial carrier-detect enum to its INI value.
 *
 * @param carrier_detect Serial carrier-detect mode.
 * @returns INI value for the carrier-detect mode.
 */
ELDER_TERMS_API std::string
serial_carrier_detect_to_string(SerialCarrierDetect carrier_detect);

/**
 * Appends serial-specific non-fatal warnings.
 *
 * @param store Source settings store.
 * @param warnings Warning sink.
 */
ELDER_TERMS_API void
append_serial_connection_warnings(const SettingsStore &store,
                                  std::vector<std::string> *warnings);

} // namespace elder_terms
