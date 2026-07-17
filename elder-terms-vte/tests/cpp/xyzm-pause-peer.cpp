#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libxyzm/sync.h>

// Test-only peer for transfer progress-bar e2e tests. It pauses a synchronous
// libxyzm transfer on request so GTK captures can inspect a stable intermediate
// progress value that was actually rendered by the application.

enum class Protocol {
  xmodem,
  ymodem,
  zmodem,
};

enum class Direction {
  send,
  receive,
};

struct Options {
  Protocol protocol = Protocol::xmodem;
  Direction direction = Direction::receive;
  std::filesystem::path source_path;
  std::filesystem::path output_dir;
  std::string fallback_name;
  std::filesystem::path pause_request_file;
  std::filesystem::path paused_file;
  std::filesystem::path resume_file;
  std::size_t pause_after_source_bytes = 0;
  uint32_t link_pace_ms = 0;
  std::size_t link_pace_every = 1;
  std::size_t max_link_chunk = 0;
};

struct PauseControl {
  std::filesystem::path pause_request_file;
  std::filesystem::path paused_file;
  std::filesystem::path resume_file;
  bool paused = false;
};

struct LinkState {
  int pace_timer_fd = -1;
  uint32_t link_pace_ms = 0;
  std::size_t link_pace_every = 1;
  std::size_t link_pace_counter = 0;
  std::size_t max_link_chunk = 0;
};

struct SourceState {
  PauseControl *pause = nullptr;
  std::filesystem::path source_path;
  std::string source_name;
  std::size_t pause_after_bytes = 0;
  bool used = false;
};

struct SourceFileState {
  int fd = -1;
  std::size_t bytes_read = 0;
};

struct SinkState {
  PauseControl *pause = nullptr;
  std::filesystem::path output_dir;
  std::string fallback_name;
};

struct SinkFileState {
  int fd = -1;
};

static constexpr uint32_t transfer_handshake_timeout_ms = 5000;
static constexpr uint32_t transfer_block_timeout_ms = 5000;
static constexpr uint32_t transfer_retry_limit = 8;
static constexpr uint8_t transfer_pad_byte = 0x1a;

static bool file_exists(const std::filesystem::path &path) {
  struct stat stat_buffer {};
  return ::stat(path.c_str(), &stat_buffer) == 0;
}

static bool write_empty_file(const std::filesystem::path &path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        0666);
  if (fd < 0) {
    return false;
  }
  return ::close(fd) == 0;
}

static xyzm_status_t wait_for_resume_file(const std::filesystem::path &path) {
  if (file_exists(path)) {
    return XYZM_STATUS_OK;
  }

  const std::filesystem::path parent = path.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path.parent_path();
  const int inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd < 0) {
    std::cerr << "failed to create inotify fd: " << std::strerror(errno)
              << '\n';
    return XYZM_STATUS_IO_ERROR;
  }

  const int watch_fd =
      ::inotify_add_watch(inotify_fd, parent.c_str(),
                          IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
  if (watch_fd < 0) {
    std::cerr << "failed to watch resume directory: " << std::strerror(errno)
              << '\n';
    (void)::close(inotify_fd);
    return XYZM_STATUS_IO_ERROR;
  }

  std::array<char, 4096> buffer {};
  while (!file_exists(path)) {
    pollfd poll_fd {
        .fd = inotify_fd,
        .events = POLLIN,
        .revents = 0,
    };
    const int selected = ::poll(&poll_fd, 1, -1);
    if (selected < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "failed to wait for resume file: " << std::strerror(errno)
                << '\n';
      (void)::inotify_rm_watch(inotify_fd, watch_fd);
      (void)::close(inotify_fd);
      return XYZM_STATUS_IO_ERROR;
    }

    for (;;) {
      const ssize_t read_size =
          ::read(inotify_fd, buffer.data(), buffer.size());
      if (read_size > 0) {
        continue;
      }
      if (read_size < 0 && errno == EINTR) {
        continue;
      }
      if (read_size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      break;
    }
  }

  (void)::inotify_rm_watch(inotify_fd, watch_fd);
  (void)::close(inotify_fd);
  return XYZM_STATUS_OK;
}

static xyzm_status_t pause_now(PauseControl *pause) {
  if (pause == nullptr) {
    return XYZM_STATUS_OK;
  }
  if (pause->paused) {
    return XYZM_STATUS_OK;
  }

  pause->paused = true;
  if (!write_empty_file(pause->paused_file)) {
    std::cerr << "failed to create paused marker: " << std::strerror(errno)
              << '\n';
    return XYZM_STATUS_IO_ERROR;
  }
  std::cerr << "xyzm-pause-peer paused" << '\n';
  const xyzm_status_t resume_status = wait_for_resume_file(pause->resume_file);
  if (resume_status == XYZM_STATUS_OK) {
    std::cerr << "xyzm-pause-peer resumed" << '\n';
  }
  return resume_status;
}

static xyzm_status_t maybe_pause(PauseControl *pause) {
  if (pause == nullptr || pause->paused ||
      !file_exists(pause->pause_request_file)) {
    return XYZM_STATUS_OK;
  }
  return pause_now(pause);
}

static uint64_t monotonic_now_ms(void *) {
  timespec now {};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * 1000u +
         static_cast<uint64_t>(now.tv_nsec / 1000000u);
}

static int poll_event_from_fd(int fd, short events, uint32_t timeout_ms) {
  pollfd poll_fd {
      .fd = fd,
      .events = events,
      .revents = 0,
  };
  for (;;) {
    const int selected =
        ::poll(&poll_fd, 1,
               timeout_ms > static_cast<uint32_t>(std::numeric_limits<int>::max())
                   ? std::numeric_limits<int>::max()
                   : static_cast<int>(timeout_ms));
    if (selected >= 0) {
      return selected;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
}

static xyzm_status_t wait_for_link_pace(LinkState *state) {
  if (state == nullptr || state->pace_timer_fd < 0 ||
      state->link_pace_ms == 0) {
    return XYZM_STATUS_OK;
  }
  ++state->link_pace_counter;
  if (state->link_pace_every > 1 &&
      state->link_pace_counter % state->link_pace_every != 0) {
    return XYZM_STATUS_OK;
  }

  // Keep the test peer slow enough for the GTK-side progress poll to observe
  // a real intermediate bar value before the transfer reaches completion.
  itimerspec timer {};
  timer.it_value.tv_sec = state->link_pace_ms / 1000u;
  timer.it_value.tv_nsec =
      static_cast<long>(state->link_pace_ms % 1000u) * 1000000L;
  if (::timerfd_settime(state->pace_timer_fd, 0, &timer, nullptr) != 0) {
    return XYZM_STATUS_IO_ERROR;
  }

  const int selected = poll_event_from_fd(state->pace_timer_fd, POLLIN,
                                          std::numeric_limits<uint32_t>::max());
  if (selected <= 0) {
    return selected == 0 ? XYZM_STATUS_TIMEOUT : XYZM_STATUS_IO_ERROR;
  }

  uint64_t expirations = 0;
  for (;;) {
    const ssize_t read_size =
        ::read(state->pace_timer_fd, &expirations, sizeof(expirations));
    if (read_size == static_cast<ssize_t>(sizeof(expirations))) {
      return XYZM_STATUS_OK;
    }
    if (read_size < 0 && errno == EINTR) {
      continue;
    }
    return XYZM_STATUS_IO_ERROR;
  }
}

static xyzm_status_t link_send(void *opaque, const uint8_t *buf, size_t len,
                               uint32_t timeout_ms, size_t *written_len) {
  auto *state = static_cast<LinkState *>(opaque);
  *written_len = 0;

  const int selected = poll_event_from_fd(STDOUT_FILENO, POLLOUT, timeout_ms);
  if (selected == 0) {
    return XYZM_STATUS_TIMEOUT;
  }
  if (selected < 0) {
    return XYZM_STATUS_IO_ERROR;
  }

  const size_t limit =
      state->max_link_chunk == 0 ? len : std::min(len, state->max_link_chunk);
  const ssize_t written = ::write(STDOUT_FILENO, buf, limit);
  if (written < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return XYZM_STATUS_TIMEOUT;
    }
    return XYZM_STATUS_IO_ERROR;
  }
  if (written == 0) {
    return XYZM_STATUS_TIMEOUT;
  }

  *written_len = static_cast<size_t>(written);
  const xyzm_status_t pace_status = wait_for_link_pace(state);
  if (pace_status != XYZM_STATUS_OK) {
    return pace_status;
  }
  return XYZM_STATUS_OK;
}

static xyzm_status_t link_recv(void *opaque, uint8_t *buf, size_t capacity,
                               uint32_t timeout_ms, size_t *read_len) {
  auto *state = static_cast<LinkState *>(opaque);
  *read_len = 0;

  const int selected = poll_event_from_fd(STDIN_FILENO, POLLIN, timeout_ms);
  if (selected == 0) {
    return XYZM_STATUS_TIMEOUT;
  }
  if (selected < 0) {
    return XYZM_STATUS_IO_ERROR;
  }

  const size_t limit = state->max_link_chunk == 0
                           ? capacity
                           : std::min(capacity, state->max_link_chunk);
  const ssize_t read_size = ::read(STDIN_FILENO, buf, limit);
  if (read_size < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return XYZM_STATUS_TIMEOUT;
    }
    return XYZM_STATUS_IO_ERROR;
  }
  if (read_size == 0) {
    return XYZM_STATUS_IO_ERROR;
  }

  *read_len = static_cast<size_t>(read_size);
  const xyzm_status_t pace_status = wait_for_link_pace(state);
  if (pace_status != XYZM_STATUS_OK) {
    return pace_status;
  }
  return XYZM_STATUS_OK;
}

static xyzm_status_t source_next(void *opaque, xyzm_file_info_t *info,
                                 void **file_opaque) {
  auto *state = static_cast<SourceState *>(opaque);
  const xyzm_status_t pause_status = maybe_pause(state->pause);
  if (pause_status != XYZM_STATUS_OK) {
    return pause_status;
  }
  if (state->used) {
    return XYZM_STATUS_END;
  }

  struct stat stat_buffer {};
  if (::stat(state->source_path.c_str(), &stat_buffer) != 0) {
    std::cerr << "failed to stat source: " << std::strerror(errno) << '\n';
    return XYZM_STATUS_IO_ERROR;
  }

  const int fd = ::open(state->source_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::cerr << "failed to open source: " << std::strerror(errno) << '\n';
    return XYZM_STATUS_IO_ERROR;
  }

  auto *file_state = new SourceFileState{.fd = fd, .bytes_read = 0};
  state->used = true;
  *info = xyzm_file_info_t{
      .valid_mask = XYZM_FILE_INFO_NAME | XYZM_FILE_INFO_SIZE,
      .name = state->source_name.c_str(),
      .size_bytes = static_cast<uint64_t>(stat_buffer.st_size),
      .mtime_unix_seconds = 0,
      .mode = 0,
  };
  *file_opaque = file_state;
  return XYZM_STATUS_OK;
}

static xyzm_status_t source_read(void *opaque, void *file_opaque,
                                 uint8_t *buf, size_t capacity,
                                 size_t *read_len) {
  auto *state = static_cast<SourceState *>(opaque);
  auto *file_state = static_cast<SourceFileState *>(file_opaque);
  *read_len = 0;
  const xyzm_status_t pause_status = maybe_pause(state->pause);
  if (pause_status != XYZM_STATUS_OK) {
    return pause_status;
  }

  if (state->pause_after_bytes > 0 &&
      file_state->bytes_read >= state->pause_after_bytes) {
    const xyzm_status_t threshold_pause_status = pause_now(state->pause);
    if (threshold_pause_status != XYZM_STATUS_OK) {
      return threshold_pause_status;
    }
  }

  size_t read_capacity = capacity;
  if (state->pause_after_bytes > file_state->bytes_read) {
    read_capacity = std::min(
        read_capacity, state->pause_after_bytes - file_state->bytes_read);
  }

  const ssize_t read_size = ::read(file_state->fd, buf, read_capacity);
  if (read_size < 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  if (read_size == 0) {
    return XYZM_STATUS_END;
  }

  *read_len = static_cast<size_t>(read_size);
  file_state->bytes_read += *read_len;
  return maybe_pause(state->pause);
}

static xyzm_status_t source_seek(void *, void *file_opaque, uint64_t offset) {
  auto *file_state = static_cast<SourceFileState *>(file_opaque);
  if (::lseek(file_state->fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
    return XYZM_STATUS_IO_ERROR;
  }
  file_state->bytes_read = static_cast<std::size_t>(offset);
  return XYZM_STATUS_OK;
}

static xyzm_status_t source_end(void *, void *file_opaque,
                                const xyzm_file_info_t *, xyzm_status_t) {
  auto *file_state = static_cast<SourceFileState *>(file_opaque);
  if (file_state != nullptr && file_state->fd >= 0) {
    (void)::close(file_state->fd);
  }
  delete file_state;
  return XYZM_STATUS_OK;
}

static std::string safe_output_name(const xyzm_file_info_t *info,
                                    const std::string &fallback_name) {
  if (info != nullptr && (info->valid_mask & XYZM_FILE_INFO_NAME) != 0 &&
      info->name != nullptr && std::strlen(info->name) > 0) {
    const std::string name =
        std::filesystem::path(info->name).filename().string();
    if (!name.empty() && name != "." && name != "..") {
      return name;
    }
  }
  return fallback_name;
}

static xyzm_status_t sink_begin(void *opaque, const xyzm_file_info_t *info,
                                uint64_t *resume_offset, void **file_opaque) {
  auto *state = static_cast<SinkState *>(opaque);
  const xyzm_status_t pause_status = maybe_pause(state->pause);
  if (pause_status != XYZM_STATUS_OK) {
    return pause_status;
  }
  std::error_code mkdir_error;
  std::filesystem::create_directories(state->output_dir, mkdir_error);
  if (mkdir_error) {
    std::cerr << "failed to create output directory: "
              << mkdir_error.message() << '\n';
    return XYZM_STATUS_IO_ERROR;
  }

  const std::filesystem::path output_path =
      state->output_dir / safe_output_name(info, state->fallback_name);
  const int fd = ::open(output_path.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (fd < 0) {
    std::cerr << "failed to open output: " << std::strerror(errno) << '\n';
    return XYZM_STATUS_IO_ERROR;
  }

  auto *file_state = new SinkFileState{.fd = fd};
  *resume_offset = 0;
  *file_opaque = file_state;
  return XYZM_STATUS_OK;
}

static xyzm_status_t write_all_to_fd(int fd, const uint8_t *buf, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    const ssize_t written = ::write(fd, buf + offset, len - offset);
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return XYZM_STATUS_IO_ERROR;
  }
  return XYZM_STATUS_OK;
}

static xyzm_status_t sink_write(void *opaque, void *file_opaque,
                                const uint8_t *buf, size_t len) {
  auto *state = static_cast<SinkState *>(opaque);
  auto *file_state = static_cast<SinkFileState *>(file_opaque);
  const xyzm_status_t pause_status = maybe_pause(state->pause);
  if (pause_status != XYZM_STATUS_OK) {
    return pause_status;
  }
  const xyzm_status_t write_status = write_all_to_fd(file_state->fd, buf, len);
  if (write_status != XYZM_STATUS_OK) {
    return write_status;
  }
  return maybe_pause(state->pause);
}

static xyzm_status_t sink_end(void *, void *file_opaque,
                              const xyzm_file_info_t *, xyzm_status_t) {
  auto *file_state = static_cast<SinkFileState *>(file_opaque);
  if (file_state != nullptr && file_state->fd >= 0) {
    (void)::close(file_state->fd);
  }
  delete file_state;
  return XYZM_STATUS_OK;
}

static xyzm_xmodem_send_opts_t xmodem_send_options() {
  return xyzm_xmodem_send_opts_t{
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .checksum_mode = XYZM_XMODEM_CHECKSUM_MODE_AUTO,
      .packet_size = XYZM_XMODEM_PACKET_SIZE_1K,
  };
}

static xyzm_xmodem_receive_opts_t xmodem_receive_options() {
  return xyzm_xmodem_receive_opts_t{
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .checksum_mode = XYZM_XMODEM_CHECKSUM_MODE_CRC,
  };
}

static xyzm_ymodem_opts_t ymodem_options() {
  return xyzm_ymodem_opts_t{
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .variant = XYZM_YMODEM_VARIANT_STANDARD,
  };
}

static xyzm_zmodem_opts_t zmodem_options() {
  return xyzm_zmodem_opts_t{
      .handshake_timeout_ms = transfer_handshake_timeout_ms,
      .block_timeout_ms = transfer_block_timeout_ms,
      .retry_limit = transfer_retry_limit,
      .pad_byte = transfer_pad_byte,
      .zmodem_packet_len = 0,
      .zmodem_escape_ctrl = 1,
      .zmodem_use_crc32 = 1,
  };
}

static std::optional<Protocol> parse_protocol(std::string_view value) {
  if (value == "xmodem") {
    return Protocol::xmodem;
  }
  if (value == "ymodem") {
    return Protocol::ymodem;
  }
  if (value == "zmodem") {
    return Protocol::zmodem;
  }
  return std::nullopt;
}

static std::optional<Direction> parse_direction(std::string_view value) {
  if (value == "send") {
    return Direction::send;
  }
  if (value == "receive") {
    return Direction::receive;
  }
  return std::nullopt;
}

static bool parse_size(std::string_view value, std::size_t *parsed) {
  if (value.empty()) {
    return false;
  }
  std::size_t result = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const std::size_t digit = static_cast<std::size_t>(ch - '0');
    if (result >
        (std::numeric_limits<std::size_t>::max() - digit) / 10u) {
      return false;
    }
    result = result * 10u + digit;
  }
  *parsed = result;
  return true;
}

static bool read_option_value(int *index, int argc, char **argv,
                              const char *name, std::string *value) {
  if (*index + 1 >= argc) {
    std::cerr << name << " requires a value" << '\n';
    return false;
  }
  *value = argv[*index + 1];
  *index += 2;
  return true;
}

static std::optional<Options> parse_options(int argc, char **argv) {
  Options options;
  bool has_protocol = false;
  bool has_direction = false;

  for (int index = 1; index < argc;) {
    const std::string argument = argv[index];
    std::string value;
    if (argument == "--protocol") {
      if (!read_option_value(&index, argc, argv, "--protocol", &value)) {
        return std::nullopt;
      }
      const auto protocol = parse_protocol(value);
      if (!protocol.has_value()) {
        std::cerr << "unknown protocol: " << value << '\n';
        return std::nullopt;
      }
      options.protocol = *protocol;
      has_protocol = true;
      continue;
    }
    if (argument == "--direction") {
      if (!read_option_value(&index, argc, argv, "--direction", &value)) {
        return std::nullopt;
      }
      const auto direction = parse_direction(value);
      if (!direction.has_value()) {
        std::cerr << "unknown direction: " << value << '\n';
        return std::nullopt;
      }
      options.direction = *direction;
      has_direction = true;
      continue;
    }
    if (argument == "--source") {
      if (!read_option_value(&index, argc, argv, "--source", &value)) {
        return std::nullopt;
      }
      options.source_path = value;
      continue;
    }
    if (argument == "--output-dir") {
      if (!read_option_value(&index, argc, argv, "--output-dir", &value)) {
        return std::nullopt;
      }
      options.output_dir = value;
      continue;
    }
    if (argument == "--fallback-name") {
      if (!read_option_value(&index, argc, argv, "--fallback-name", &value)) {
        return std::nullopt;
      }
      options.fallback_name = value;
      continue;
    }
    if (argument == "--pause-request-file") {
      if (!read_option_value(&index, argc, argv, "--pause-request-file",
                             &value)) {
        return std::nullopt;
      }
      options.pause_request_file = value;
      continue;
    }
    if (argument == "--paused-file") {
      if (!read_option_value(&index, argc, argv, "--paused-file", &value)) {
        return std::nullopt;
      }
      options.paused_file = value;
      continue;
    }
    if (argument == "--resume-file") {
      if (!read_option_value(&index, argc, argv, "--resume-file", &value)) {
        return std::nullopt;
      }
      options.resume_file = value;
      continue;
    }
    if (argument == "--pause-after-source-bytes") {
      if (!read_option_value(&index, argc, argv,
                             "--pause-after-source-bytes", &value) ||
          !parse_size(value, &options.pause_after_source_bytes) ||
          options.pause_after_source_bytes == 0) {
        std::cerr
            << "--pause-after-source-bytes requires a positive integer"
            << '\n';
        return std::nullopt;
      }
      continue;
    }
    if (argument == "--max-link-chunk") {
      if (!read_option_value(&index, argc, argv, "--max-link-chunk", &value) ||
          !parse_size(value, &options.max_link_chunk) ||
          options.max_link_chunk == 0) {
        std::cerr << "--max-link-chunk requires a positive integer" << '\n';
        return std::nullopt;
      }
      continue;
    }
    if (argument == "--link-pace-ms") {
      std::size_t parsed = 0;
      if (!read_option_value(&index, argc, argv, "--link-pace-ms", &value) ||
          !parse_size(value, &parsed) ||
          parsed > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "--link-pace-ms requires a uint32 integer" << '\n';
        return std::nullopt;
      }
      options.link_pace_ms = static_cast<uint32_t>(parsed);
      continue;
    }
    if (argument == "--link-pace-every") {
      if (!read_option_value(&index, argc, argv, "--link-pace-every", &value) ||
          !parse_size(value, &options.link_pace_every) ||
          options.link_pace_every == 0) {
        std::cerr << "--link-pace-every requires a positive integer" << '\n';
        return std::nullopt;
      }
      continue;
    }

    std::cerr << "unknown option: " << argument << '\n';
    return std::nullopt;
  }

  if (!has_protocol || !has_direction ||
      options.pause_request_file.empty() || options.paused_file.empty() ||
      options.resume_file.empty()) {
    std::cerr << "missing required peer options" << '\n';
    return std::nullopt;
  }
  if (options.direction == Direction::send && options.source_path.empty()) {
    std::cerr << "--source is required for --direction send" << '\n';
    return std::nullopt;
  }
  if (options.direction == Direction::receive &&
      (options.output_dir.empty() || options.fallback_name.empty())) {
    std::cerr
        << "--output-dir and --fallback-name are required for --direction receive"
        << '\n';
    return std::nullopt;
  }

  return options;
}

static xyzm_status_t run_send(const Options &options,
                              const xyzm_link_ops_t &link,
                              PauseControl *pause) {
  SourceState source_state{
      .pause = pause,
      .source_path = options.source_path,
      .source_name = options.source_path.filename().string(),
      .pause_after_bytes = options.pause_after_source_bytes,
      .used = false,
  };
  xyzm_source_ops_t source{
      .opaque = &source_state,
      .next = source_next,
      .read = source_read,
      .seek = source_seek,
      .end = source_end,
  };
  xyzm_transfer_report_t report {};

  if (options.protocol == Protocol::xmodem) {
    xyzm_xmodem_send_opts_t opts = xmodem_send_options();
    xyzm_xmodem_send_request_t request{
        .link = &link,
        .source = &source,
        .options = &opts,
        .observer = nullptr,
    };
    return xyzm_xmodem_send(&request, &report);
  }
  if (options.protocol == Protocol::ymodem) {
    xyzm_ymodem_opts_t opts = ymodem_options();
    xyzm_ymodem_send_request_t request{
        .link = &link,
        .source = &source,
        .options = &opts,
        .observer = nullptr,
    };
    return xyzm_ymodem_send(&request, &report);
  }

  xyzm_zmodem_opts_t opts = zmodem_options();
  xyzm_zmodem_send_request_t request{
      .link = &link,
      .source = &source,
      .options = &opts,
      .observer = nullptr,
  };
  return xyzm_zmodem_send(&request, &report);
}

static xyzm_status_t run_receive(const Options &options,
                                 const xyzm_link_ops_t &link,
                                 PauseControl *pause) {
  SinkState sink_state{
      .pause = pause,
      .output_dir = options.output_dir,
      .fallback_name = options.fallback_name,
  };
  xyzm_sink_ops_t sink{
      .opaque = &sink_state,
      .begin = sink_begin,
      .write = sink_write,
      .end = sink_end,
  };
  xyzm_transfer_report_t report {};

  if (options.protocol == Protocol::xmodem) {
    xyzm_xmodem_receive_opts_t opts = xmodem_receive_options();
    xyzm_xmodem_receive_request_t request{
        .link = &link,
        .sink = &sink,
        .options = &opts,
        .observer = nullptr,
    };
    return xyzm_xmodem_receive(&request, &report);
  }
  if (options.protocol == Protocol::ymodem) {
    xyzm_ymodem_opts_t opts = ymodem_options();
    xyzm_ymodem_receive_request_t request{
        .link = &link,
        .sink = &sink,
        .options = &opts,
        .observer = nullptr,
    };
    return xyzm_ymodem_receive(&request, &report);
  }

  xyzm_zmodem_opts_t opts = zmodem_options();
  xyzm_zmodem_receive_request_t request{
      .link = &link,
      .sink = &sink,
      .options = &opts,
      .observer = nullptr,
  };
  return xyzm_zmodem_receive(&request, &report);
}

int main(int argc, char **argv) {
  const std::optional<Options> parsed_options = parse_options(argc, argv);
  if (!parsed_options.has_value()) {
    return 2;
  }
  const Options &options = *parsed_options;

  PauseControl pause{
      .pause_request_file = options.pause_request_file,
      .paused_file = options.paused_file,
      .resume_file = options.resume_file,
      .paused = false,
  };
  LinkState link_state{
      .pace_timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC),
      .link_pace_ms = options.link_pace_ms,
      .link_pace_every = options.link_pace_every,
      .link_pace_counter = 0,
      .max_link_chunk = options.max_link_chunk,
  };
  if (link_state.pace_timer_fd < 0) {
    std::cerr << "failed to create pace timer: " << std::strerror(errno)
              << '\n';
    return 1;
  }
  xyzm_link_ops_t link{
      .opaque = &link_state,
      .send = link_send,
      .recv = link_recv,
      .now_ms = monotonic_now_ms,
  };

  const xyzm_status_t status =
      options.direction == Direction::send
          ? run_send(options, link, &pause)
          : run_receive(options, link, &pause);
  if (status != XYZM_STATUS_OK) {
    std::cerr << "xyzm-pause-peer failed: " << static_cast<int>(status)
              << '\n';
    (void)::close(link_state.pace_timer_fd);
    return 1;
  }

  (void)::close(link_state.pace_timer_fd);
  return 0;
}
