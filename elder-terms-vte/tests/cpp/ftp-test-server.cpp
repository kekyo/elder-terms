#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace elder_terms_ftp_test_server {

struct Socket {
  int fd = -1;

  explicit Socket(int fd = -1) : fd(fd) {}
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
  Socket &operator=(Socket &&other) noexcept {
    if (fd >= 0) ::close(fd);
    fd = std::exchange(other.fd, -1);
    return *this;
  }
  ~Socket() { if (fd >= 0) ::close(fd); }
};

struct Options {
  std::filesystem::path root;
  bool ipv6 = false;
  bool legacy_data = false;
  bool reject_login = false;
  bool hold_first_list = false;
  bool foreign_active = false;
  bool trace = false;
  bool hold_final = false;
  bool hold_upload = false;
  bool reject_store = false;
  bool final_error = false;
  bool final_disconnect = false;
  bool truncated_download = false;
  bool large_size = false;
  std::string listing = "mlsd";
  int mlsd_error = 0;
};

static void expect(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

static void send_text(int fd, std::string_view text) {
  while (!text.empty()) {
    const auto count = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    expect(count > 0, "FTP test server send failed");
    text.remove_prefix(static_cast<std::size_t>(count));
  }
}

static std::string read_line(int fd) {
  std::string text;
  char ch;
  for (;;) {
    const auto count = ::read(fd, &ch, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return {};
    if (ch == '\n') {
      if (!text.empty() && text.back() == '\r') text.pop_back();
      return text;
    }
    text += ch;
    expect(text.size() <= 65536, "FTP test command exceeds limit");
  }
}

static std::pair<Socket, unsigned> listen_local(bool ipv6) {
  Socket socket(::socket(ipv6 ? AF_INET6 : AF_INET,
                         SOCK_STREAM | SOCK_CLOEXEC, 0));
  expect(socket.fd >= 0, "FTP test socket failed");
  sockaddr_storage storage{};
  socklen_t length;
  if (ipv6) {
    auto &address = reinterpret_cast<sockaddr_in6 &>(storage);
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    length = sizeof(address);
  } else {
    auto &address = reinterpret_cast<sockaddr_in &>(storage);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    length = sizeof(address);
  }
  expect(::bind(socket.fd, reinterpret_cast<sockaddr *>(&storage), length) == 0 &&
             ::listen(socket.fd, 4) == 0,
         "FTP test listen failed");
  expect(::getsockname(socket.fd, reinterpret_cast<sockaddr *>(&storage), &length) == 0,
         "FTP test getsockname failed");
  const unsigned port = ipv6
      ? ntohs(reinterpret_cast<sockaddr_in6 &>(storage).sin6_port)
      : ntohs(reinterpret_cast<sockaddr_in &>(storage).sin_port);
  return {std::move(socket), port};
}

static std::string quote_path(std::string_view path) {
  std::string result = "\"";
  for (const char ch : path) {
    result += ch;
    if (ch == '"') result += '"';
  }
  return result + '"';
}

static std::string facts(const std::filesystem::path &path,
                          const std::string &name) {
  const bool directory = std::filesystem::is_directory(path);
  return std::string("type=") + (directory ? "dir" : "file") +
      ";size=" + std::to_string(directory ? 0 : std::filesystem::file_size(path)) +
      ";modify=20240102030405; " + name + "\r\n";
}

static void serve(int control, Options options) {
  std::filesystem::path cwd = "/home";
  std::filesystem::path rename_from;
  Socket passive;
  sockaddr_storage active{};
  socklen_t active_length = 0;
  bool logged_in = false;
  bool valid_user = false;
  const auto remote_path = [&](const std::string &argument) {
    const std::filesystem::path path = argument.empty() ? cwd : std::filesystem::path(argument);
    auto resolved = (path.is_absolute() ? path : cwd / path).lexically_normal();
    if (resolved != "/" && resolved.filename().empty()) resolved = resolved.parent_path();
    return resolved;
  };
  const auto local_path = [&](const std::filesystem::path &path) {
    return options.root / path.relative_path();
  };
  const auto respond = [&](int code, const std::string &message) {
    send_text(control, std::to_string(code) + " " + message + "\r\n");
  };
  send_text(control, "220 Local filesystem FTP test server\r\n");
  for (;;) {
    const auto command = read_line(control);
    if (command.empty()) return;
    const auto separator = command.find(' ');
    const auto verb = command.substr(0, separator);
    const auto argument = separator == std::string::npos ? std::string() : command.substr(separator + 1);
    // Credentials must never appear in test evidence.
    if (options.trace) {
      std::clog << "COMMAND " << (verb == "PASS" ? "PASS [hidden]" : command) << std::endl;
    }
    if (verb == "USER") {
      valid_user = argument == "alice";
      respond(valid_user ? 331 : 530, "Identity checked");
    } else if (verb == "PASS") {
      logged_in = valid_user && argument == "secret" && !options.reject_login;
      respond(logged_in ? 230 : 530, "Password checked");
    } else if (verb == "QUIT") {
      respond(221, "Goodbye");
      return;
    } else if (!logged_in) {
      respond(530, "Login required");
    } else if (verb == "PWD") {
      respond(257, quote_path(cwd.string()));
    } else if (verb == "CWD") {
      auto target = remote_path(argument);
      if (target == "/alias") target = "/home";
      if (target.filename() == "denied" || !std::filesystem::is_directory(local_path(target))) {
        respond(550, "Directory unavailable");
      } else {
        // Hold the location probe so queued mutations would become visible in
        // the later listing if the client released its logical operation lock.
        if (options.hold_first_list) {
          options.hold_first_list = false;
          std::cout << "LIST_WAIT" << std::endl;
          expect(read_line(STDIN_FILENO) == "release", "Expected listing release");
        }
        cwd = target;
        respond(250, "Directory changed");
      }
    } else if (verb == "FEAT") {
      send_text(control, options.listing == "mlsd"
          ? "211-Extensions\r\n MLST type*;size*;modify*;\r\n UTF8\r\n211 End\r\n"
          : "500 FEAT unavailable\r\n");
    } else if (verb == "OPTS" || verb == "TYPE") {
      respond(200, "Option accepted");
    } else if (verb == "EPSV" || verb == "PASV") {
      if (verb == "EPSV" && options.legacy_data) {
        respond(500, "EPSV unavailable");
        continue;
      }
      auto [socket, port] = listen_local(options.ipv6);
      passive = std::move(socket);
      active_length = 0;
      if (verb == "EPSV") {
        respond(229, "(|||" + std::to_string(port) + "|)");
      } else {
        // A deliberately unrelated address verifies FTP_SKIP_PASV_IP.
        respond(227, "(127,0,0,2," + std::to_string(port / 256) + "," + std::to_string(port % 256) + ")");
      }
    } else if (verb == "EPRT") {
      if (options.legacy_data) {
        respond(500, "EPRT unavailable");
        continue;
      }
      expect(!argument.empty(), "Missing EPRT endpoint");
      const char delimiter = argument.front();
      const auto split1 = argument.find(delimiter, 1);
      const auto split2 = argument.find(delimiter, split1 + 1);
      const auto split3 = argument.find(delimiter, split2 + 1);
      const auto host = argument.substr(split1 + 1, split2 - split1 - 1);
      const auto port = std::stoul(argument.substr(split2 + 1, split3 - split2 - 1));
      const bool ipv6 = argument.substr(1, split1 - 1) == "2";
      active = {};
      if (ipv6) {
        auto &address = reinterpret_cast<sockaddr_in6 &>(active);
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        expect(::inet_pton(AF_INET6, host.c_str(), &address.sin6_addr) == 1, "Invalid EPRT IPv6");
        active_length = sizeof(address);
      } else {
        auto &address = reinterpret_cast<sockaddr_in &>(active);
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        expect(::inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1, "Invalid EPRT IPv4");
        active_length = sizeof(address);
      }
      passive = Socket();
      respond(200, "Active endpoint accepted");
    } else if (verb == "PORT") {
      auto numbers = argument;
      std::replace(numbers.begin(), numbers.end(), ',', ' ');
      std::istringstream input(numbers);
      std::array<unsigned, 6> parts{};
      for (auto &part : parts) expect(bool(input >> part) && part <= 255, "Invalid PORT endpoint");
      active = {};
      auto &address = reinterpret_cast<sockaddr_in &>(active);
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
      address.sin_port = htons(parts[4] * 256 + parts[5]);
      active_length = sizeof(address);
      passive = Socket();
      respond(200, "Active endpoint accepted");
    } else if (verb == "MLSD" || verb == "LIST") {
      if (verb == "MLSD" && options.mlsd_error != 0) {
        respond(options.mlsd_error, "MLSD rejected");
        passive = Socket();
        continue;
      }
      respond(150, "Opening listing");
      Socket data;
      if (passive.fd >= 0) {
        data = Socket(::accept4(passive.fd, nullptr, nullptr, SOCK_CLOEXEC));
        passive = Socket();
      } else {
        expect(active_length > 0, "Missing active endpoint");
        data = Socket(::socket(active.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (options.foreign_active) {
          sockaddr_in source{};
          source.sin_family = AF_INET;
          source.sin_addr.s_addr = htonl(0x7f000002);
          expect(::bind(data.fd, reinterpret_cast<sockaddr *>(&source), sizeof(source)) == 0,
                 "Foreign active data address could not be bound");
        }
        expect(::connect(data.fd, reinterpret_cast<sockaddr *>(&active), active_length) == 0, "Active connection failed");
      }
      expect(data.fd >= 0, "Listing connection failed");
      std::string listing;
      if (verb == "MLSD") listing = "type=cdir; .\r\ntype=pdir; ..\r\n";
      else if (options.listing != "dos") listing = "total 0\r\n";
      for (const auto &entry : std::filesystem::directory_iterator(local_path(cwd))) {
        const auto name = entry.path().filename().string();
        const bool directory = entry.is_directory();
        if (verb == "MLSD") {
          listing += facts(entry.path(), name);
        } else if (options.listing == "dos") {
          listing += "01-02-24  03:04AM " + (directory ? std::string("<DIR>") : std::to_string(entry.file_size())) + " " + name + "\r\n";
        } else {
          listing += std::string(directory ? "drwxr-xr-x" : "-rw-r--r--") + " 1 owner group " +
              std::to_string(directory ? 0 : entry.file_size()) + " Jan 02 2024 " + name + "\r\n";
        }
      }
      send_text(data.fd, listing);
      data = Socket();
      respond(226, "Listing complete");
    } else if (verb == "SIZE") {
      const auto path = local_path(remote_path(argument));
      if (std::filesystem::is_regular_file(path)) {
        respond(213, std::to_string(options.large_size ? 4294967313ULL
                                                     : std::filesystem::file_size(path)));
      } else {
        respond(550, "File unavailable");
      }
    } else if (verb == "RETR" || verb == "STOR") {
      const auto path = local_path(remote_path(argument));
      const bool upload = verb == "STOR";
      if ((upload && options.reject_store) ||
          (!upload && !std::filesystem::is_regular_file(path))) {
        passive = Socket();
        respond(550, "Transfer refused");
        continue;
      }
      std::fstream file(path, std::ios::binary |
          (upload ? std::ios::out | std::ios::trunc : std::ios::in));
      if (!file) {
        passive = Socket();
        respond(550, "File unavailable");
        continue;
      }
      respond(150, "Opening binary transfer");
      Socket data;
      if (passive.fd >= 0) {
        data = Socket(::accept4(passive.fd, nullptr, nullptr, SOCK_CLOEXEC));
        passive = Socket();
      } else {
        expect(active_length > 0, "Missing active endpoint");
        data = Socket(::socket(active.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (options.foreign_active) {
          sockaddr_in source{};
          source.sin_family = AF_INET;
          source.sin_addr.s_addr = htonl(0x7f000002);
          expect(::bind(data.fd, reinterpret_cast<sockaddr *>(&source), sizeof(source)) == 0,
                 "Foreign active data address could not be bound");
        }
        expect(::connect(data.fd, reinterpret_cast<sockaddr *>(&active), active_length) == 0,
               "Active transfer connection failed");
      }
      expect(data.fd >= 0, "Transfer connection failed");
      if (upload && options.hold_upload) {
        std::cout << "DATA_WAIT" << std::endl;
        const auto action = read_line(STDIN_FILENO);
        if (action == "cancel") {
          auto closing = read_line(control);
          if (closing == "QUIT") closing = read_line(control);
          expect(closing.empty(), "Canceled upload must close control");
          return;
        }
        expect(action == "release", "Expected data release");
      }
      std::array<char, 16384> buffer;
      if (upload) {
        for (;;) {
          const auto count = ::read(data.fd, buffer.data(), buffer.size());
          if (count < 0 && errno == EINTR) continue;
          expect(count >= 0, "Upload data read failed");
          if (count == 0) break;
          file.write(buffer.data(), count);
          expect(bool(file), "Upload file write failed");
        }
      } else {
        auto remaining = std::filesystem::file_size(path);
        if (options.truncated_download) remaining /= 2;
        while (remaining > 0) {
          const auto count = std::min<std::uintmax_t>(remaining, buffer.size());
          file.read(buffer.data(), static_cast<std::streamsize>(count));
          expect(file.gcount() == static_cast<std::streamsize>(count), "Download file read failed");
          send_text(data.fd, {buffer.data(), static_cast<std::size_t>(count)});
          remaining -= count;
        }
      }
      file.close();
      data = Socket();
      if (options.hold_final) {
        std::cout << "FINAL_WAIT" << std::endl;
        const auto action = read_line(STDIN_FILENO);
        if (action == "cancel") {
          auto closing = read_line(control);
          if (closing == "QUIT") closing = read_line(control);
          expect(closing.empty(), "Canceled transfer must close control");
          return;
        }
        expect(action == "release", "Expected final response release");
      }
      if (options.final_disconnect) return;
      respond(options.final_error ? 451 : 226, "Transfer finished");
    } else if (verb == "MLST") {
      const auto target = remote_path(argument);
      if (!std::filesystem::exists(local_path(target)) || target.filename() == "denied") {
        respond(550, "Item unavailable");
      } else {
        send_text(control, "250-Facts\r\n " + facts(local_path(target), target.string()) + "250 End\r\n");
      }
    } else if (verb == "MKD") {
      const auto target = local_path(remote_path(argument));
      std::error_code error;
      respond(std::filesystem::create_directory(target, error) ? 257 : 550, quote_path(argument));
    } else if (verb == "DELE" || verb == "RMD") {
      const auto target = local_path(remote_path(argument));
      std::error_code error;
      const bool allowed = target.filename() != "denied" &&
          std::filesystem::is_directory(target) == (verb == "RMD");
      respond(allowed && std::filesystem::remove(target, error) ? 250 : 550, "Removal result");
    } else if (verb == "RNFR") {
      rename_from = local_path(remote_path(argument));
      respond(std::filesystem::exists(rename_from) ? 350 : 550, "Rename source");
    } else if (verb == "RNTO") {
      const auto target = local_path(remote_path(argument));
      std::error_code error;
      if (target.filename() == "denied") {
        respond(550, "Rename denied");
      } else {
        std::filesystem::rename(rename_from, target, error);
        respond(error ? 550 : 250, "Rename result");
      }
      rename_from.clear();
    } else {
      respond(502, "Command unsupported");
    }
  }
}

} // namespace elder_terms_ftp_test_server

int main(int argc, char **argv) {
  using namespace elder_terms_ftp_test_server;
  try {
    expect(argc >= 2, "Usage: ftp-test-server ROOT [options]");
    Options options;
    options.root = argv[1];
    for (int index = 2; index < argc; ++index) {
      const std::string option = argv[index];
      if (option == "--ipv6") options.ipv6 = true;
      else if (option == "--legacy-data") options.legacy_data = true;
      else if (option == "--reject-login") options.reject_login = true;
      else if (option == "--hold-first-list") options.hold_first_list = true;
      else if (option == "--foreign-active") options.foreign_active = true;
      else if (option == "--trace") options.trace = true;
      else if (option == "--hold-final") options.hold_final = true;
      else if (option == "--hold-upload") options.hold_upload = true;
      else if (option == "--reject-store") options.reject_store = true;
      else if (option == "--final-error") options.final_error = true;
      else if (option == "--final-disconnect") options.final_disconnect = true;
      else if (option == "--truncated-download") options.truncated_download = true;
      else if (option == "--large-size") options.large_size = true;
      else if (option == "--unix") options.listing = "unix";
      else if (option == "--dos") options.listing = "dos";
      else if (option == "--mlsd-unavailable") options.mlsd_error = 502;
      else if (option == "--mlsd-denied") options.mlsd_error = 550;
      else if (option == "--mlsd-temporary") options.mlsd_error = 450;
      else throw std::runtime_error("Unknown FTP test option: " + option);
    }
    auto [listener, port] = listen_local(options.ipv6);
    std::cout << "READY " << port << std::endl;
    for (;;) {
      Socket control(::accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC));
      if (control.fd < 0 && errno == EINTR) continue;
      expect(control.fd >= 0, "FTP test accept failed");
      // A refused operation may make libcurl reconnect. Filesystem behavior
      // belongs to the session, rather than to one physical TCP connection.
      serve(control.fd, options);
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
