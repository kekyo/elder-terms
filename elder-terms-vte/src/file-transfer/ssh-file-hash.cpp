#include "file-hash.h"

#include "../terminal-sessions/ssh-session/authenticated-ssh-transport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace elder_terms {

static std::string quote_shell_argument(const std::string &value) {
  std::string result = "'";
  for (const char character : value) {
    if (character == '\'') {
      result += "'\"'\"'";
    } else {
      result += character;
    }
  }
  result += "'";
  return result;
}

static std::string parse_hash_line(const std::string &line,
                                   std::size_t expected_size,
                                   const char *algorithm) {
  std::istringstream input(line);
  std::string value;
  input >> value;
  const bool valid =
      value.size() == expected_size &&
      std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
      });
  if (!valid) {
    throw std::runtime_error(std::string("Invalid ") + algorithm +
                             " hash returned by SSH server");
  }
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

cardio::promise<FileHashes> calculate_ssh_file_hashes_async(
    std::shared_ptr<AuthenticatedSshTransport> transport, std::string path,
    cardio::cancellation cancellation) {
  if (transport == nullptr) {
    throw std::invalid_argument("SSH transport is required");
  }
  if (path.empty()) {
    throw std::invalid_argument("Remote file path is required");
  }

  const std::string argument = quote_shell_argument(path);
  const std::string command =
      "LC_ALL=C md5sum < " + argument +
      " && LC_ALL=C sha1sum < " + argument +
      " && LC_ALL=C sha256sum < " + argument;
  const SshCommandResult result = co_await transport->execute_command_async(
      command, std::move(cancellation));
  if (result.exit_status != 0) {
    std::string message = "Remote hash command failed with exit status " +
                          std::to_string(result.exit_status);
    if (!result.standard_error.empty()) {
      message += ": " + result.standard_error;
    }
    throw std::runtime_error(message);
  }

  std::istringstream output(result.standard_output);
  std::array<std::string, 3> lines{};
  std::size_t line_count = 0;
  std::string line;
  while (std::getline(output, line)) {
    if (line.empty()) {
      continue;
    }
    if (line_count >= lines.size()) {
      throw std::runtime_error(
          "Unexpected output returned by remote hash command");
    }
    lines[line_count++] = std::move(line);
  }
  if (line_count != lines.size()) {
    throw std::runtime_error(
        "Incomplete output returned by remote hash command");
  }
  co_return FileHashes{
      .md5 = parse_hash_line(lines[0], 32, "MD5"),
      .sha1 = parse_hash_line(lines[1], 40, "SHA-1"),
      .sha256 = parse_hash_line(lines[2], 64, "SHA-256"),
  };
}

} // namespace elder_terms
