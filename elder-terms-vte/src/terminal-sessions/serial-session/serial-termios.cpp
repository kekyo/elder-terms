#include "serial-termios.h"

#include <sys/ioctl.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace elder_terms {

#ifndef BOTHER
static constexpr tcflag_t BOTHER = 0010000;
#endif

#ifndef CBAUD
static constexpr tcflag_t CBAUD = 0010017;
#endif

struct LinuxTermios2 {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[19];
  speed_t c_ispeed;
  speed_t c_ospeed;
};

static constexpr unsigned long linux_tcgets2_request =
    _IOR('T', 0x2A, LinuxTermios2);
static constexpr unsigned long linux_tcsets2_request =
    _IOW('T', 0x2B, LinuxTermios2);

static speed_t standard_baudrate(gint64 baudrate, bool *found) {
  *found = true;
  switch (baudrate) {
  case 150:
    return B150;
  case 300:
    return B300;
  case 600:
    return B600;
  case 1200:
    return B1200;
  case 1800:
    return B1800;
  case 2400:
    return B2400;
  case 4800:
    return B4800;
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
#ifdef B57600
  case 57600:
    return B57600;
#endif
#ifdef B115200
  case 115200:
    return B115200;
#endif
#ifdef B230400
  case 230400:
    return B230400;
#endif
#ifdef B460800
  case 460800:
    return B460800;
#endif
#ifdef B500000
  case 500000:
    return B500000;
#endif
#ifdef B576000
  case 576000:
    return B576000;
#endif
#ifdef B921600
  case 921600:
    return B921600;
#endif
#ifdef B1000000
  case 1000000:
    return B1000000;
#endif
#ifdef B1152000
  case 1152000:
    return B1152000;
#endif
#ifdef B1500000
  case 1500000:
    return B1500000;
#endif
#ifdef B2000000
  case 2000000:
    return B2000000;
#endif
#ifdef B2500000
  case 2500000:
    return B2500000;
#endif
#ifdef B3000000
  case 3000000:
    return B3000000;
#endif
#ifdef B3500000
  case 3500000:
    return B3500000;
#endif
#ifdef B4000000
  case 4000000:
    return B4000000;
#endif
  default:
    *found = false;
    return B38400;
  }
}

static tcflag_t bits_flag(gint64 bits) {
  if (bits == 5) {
    return CS5;
  }
  if (bits == 6) {
    return CS6;
  }
  if (bits == 7) {
    return CS7;
  }
  return CS8;
}

static void apply_custom_baudrate(int fd, gint64 baudrate) {
  LinuxTermios2 attributes{};
  if (::ioctl(fd, linux_tcgets2_request, &attributes) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "serial TCGETS2 failed");
  }

  attributes.c_cflag &= ~CBAUD;
  attributes.c_cflag |= BOTHER;
  attributes.c_ispeed = static_cast<speed_t>(baudrate);
  attributes.c_ospeed = static_cast<speed_t>(baudrate);
  if (::ioctl(fd, linux_tcsets2_request, &attributes) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "serial TCSETS2 failed");
  }
}

SerialTermiosConfiguration
serial_termios_configuration(SerialConnectionSettings settings) {
  bool found_standard_baudrate = false;
  const speed_t standard_speed =
      standard_baudrate(settings.baudrate, &found_standard_baudrate);

  SerialTermiosConfiguration configuration{
      .control_clear = CSIZE | PARENB | PARODD | CSTOPB,
      .control_set = CLOCAL | CREAD | bits_flag(settings.bits),
      .input_clear = IXON | IXOFF | IXANY,
      .input_set = 0,
      .standard_speed = standard_speed,
      .custom_baudrate = !found_standard_baudrate,
      .baudrate = settings.baudrate,
  };

  if (settings.parity == SerialParity::even) {
    configuration.control_set |= PARENB;
  } else if (settings.parity == SerialParity::odd) {
    configuration.control_set |= PARENB | PARODD;
  }

  if (settings.stop_bit == 2) {
    configuration.control_set |= CSTOPB;
  }

#ifdef CRTSCTS
  configuration.control_clear |= CRTSCTS;
  if (settings.flow_control == SerialFlowControl::hard) {
    configuration.control_set |= CRTSCTS;
  }
#else
  if (settings.flow_control == SerialFlowControl::hard) {
    throw std::runtime_error(
        "serial hardware flow control is not supported on this platform");
  }
#endif

  if (settings.flow_control == SerialFlowControl::xon) {
    configuration.input_set |= IXON | IXOFF;
  }

  return configuration;
}

void configure_serial_port(int fd, SerialConnectionSettings settings) {
  termios attributes{};
  if (::tcgetattr(fd, &attributes) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "serial tcgetattr failed");
  }

  const SerialTermiosConfiguration configuration =
      serial_termios_configuration(settings);
  ::cfmakeraw(&attributes);
  attributes.c_cflag &= ~configuration.control_clear;
  attributes.c_cflag |= configuration.control_set;
  attributes.c_iflag &= ~configuration.input_clear;
  attributes.c_iflag |= configuration.input_set;

  if (::cfsetispeed(&attributes, configuration.standard_speed) < 0 ||
      ::cfsetospeed(&attributes, configuration.standard_speed) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "serial cfset speed failed");
  }

  if (::tcsetattr(fd, TCSANOW, &attributes) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "serial tcsetattr failed");
  }

  if (configuration.custom_baudrate) {
    apply_custom_baudrate(fd, configuration.baudrate);
  }
}

} // namespace elder_terms
