#include "../../src/ftp/ftp-protocol.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace elder_terms {

static void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Operation>
static void expect_failure(Operation operation, const char *message) {
  try {
    operation();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}

static FtpReply parse_one_reply(std::string_view text) {
  FtpReplyParser parser;
  parser.feed(text);
  parser.finish();
  std::optional<FtpReply> reply = parser.take_reply();
  expect(reply.has_value(), "FTP parser should produce one reply");
  expect(!parser.take_reply().has_value(),
         "FTP parser should not produce an extra reply");
  return std::move(*reply);
}

static void fragmented_and_pipelined_replies_are_parsed() {
  FtpReplyParser parser;
  parser.feed("22");
  expect(!parser.take_reply().has_value(),
         "partial FTP reply should remain pending");
  parser.feed("0 Ready\r\n200 First command OK\r\n");

  const std::optional<FtpReply> greeting = parser.take_reply();
  const std::optional<FtpReply> command = parser.take_reply();
  expect(greeting.has_value() && greeting->code == 220 &&
             greeting->lines == std::vector<std::string>{"220 Ready"},
         "fragmented FTP greeting should be reconstructed");
  expect(command.has_value() && command->code == 200,
         "pipelined FTP reply should be retained");
}

static void multiline_reply_requires_the_matching_terminator() {
  FtpReplyParser parser;
  parser.feed("211-Features\r\n UTF8\r\n200 Not the terminator\r\n");
  expect(!parser.take_reply().has_value(),
         "another reply code must not terminate a multiline reply");
  parser.feed("211 End\r\n");

  const std::optional<FtpReply> reply = parser.take_reply();
  expect(reply.has_value() && reply->code == 211 &&
             reply->lines ==
                 std::vector<std::string>{
                     "211-Features", " UTF8", "200 Not the terminator",
                     "211 End"},
         "FTP multiline reply should preserve every line");
}

static void malformed_or_truncated_replies_are_rejected() {
  expect_failure(
      []() { (void)parse_one_reply("ready\r\n"); },
      "FTP reply without a status code should fail");
  expect_failure(
      []() { (void)parse_one_reply("220-Ready\r\n"); },
      "unterminated FTP multiline reply should fail");

  FtpReplyParser parser;
  expect_failure(
      [&parser]() { parser.feed(std::string(9000, 'x')); },
      "oversized FTP reply line should fail before unbounded buffering");
}

static void passive_responses_are_validated() {
  expect(parse_ftp_epsv_port(
             parse_one_reply("229 Entering Extended Passive Mode (|||6446|)\r\n")) ==
             6446,
         "EPSV response should expose its port");
  const FtpPassiveEndpoint endpoint = parse_ftp_pasv_endpoint(
      parse_one_reply("227 Entering Passive Mode (192,0,2,10,195,80)\r\n"));
  expect(endpoint.address == "192.0.2.10" && endpoint.port == 50000,
         "PASV response should expose IPv4 address and port");

  expect_failure(
      []() {
        (void)parse_ftp_epsv_port(
            parse_one_reply("229 Passive (|||0|)\r\n"));
      },
      "EPSV port zero should fail");
  expect_failure(
      []() {
        (void)parse_ftp_pasv_endpoint(
            parse_one_reply("227 Passive (192,0,2,999,1,2)\r\n"));
      },
      "PASV octets outside one byte should fail");
}

static void pwd_response_unescapes_embedded_quotes() {
  expect(parse_ftp_pwd_path(
             parse_one_reply("257 \"/reports/\"\"quoted\"\"\" is current\r\n")) ==
             "/reports/\"quoted\"",
         "PWD should decode doubled quote characters");
  expect_failure(
      []() {
        (void)parse_ftp_pwd_path(
            parse_one_reply("257 current directory unavailable\r\n"));
      },
      "PWD reply without a quoted pathname should fail");
}

static void mlsd_entries_preserve_machine_readable_facts() {
  const std::optional<FtpDirectoryEntry> file = parse_ftp_mlsd_entry(
      "Type=file;Size=42;Modify=19700101000000; report final.txt");
  expect(file.has_value() && file->name == "report final.txt" &&
             file->type == FtpDirectoryEntryType::regular &&
             file->size == 42 &&
             file->modification_time_unix_seconds == 0,
         "MLSD should parse case-insensitive standard facts");

  const std::optional<FtpDirectoryEntry> directory =
      parse_ftp_mlsd_entry("type=dir;modify=20240229010203; archive");
  expect(directory.has_value() &&
             directory->type == FtpDirectoryEntryType::directory &&
             directory->modification_time_unix_seconds.has_value(),
         "MLSD should parse a valid leap-day timestamp");

  const std::optional<FtpDirectoryEntry> current =
      parse_ftp_mlsd_entry("type=cdir; /");
  const std::optional<FtpDirectoryEntry> parent =
      parse_ftp_mlsd_entry("type=pdir; ..");
  expect(current.has_value() &&
             current->type == FtpDirectoryEntryType::current_directory &&
             parent.has_value() &&
             parent->type == FtpDirectoryEntryType::parent_directory,
         "MLSD should distinguish current and parent pseudo entries");

  const std::optional<FtpDirectoryEntry> invalid_time =
      parse_ftp_mlsd_entry("type=file;size=7;modify=20230229010203; item");
  expect(invalid_time.has_value() &&
             !invalid_time->modification_time_unix_seconds.has_value(),
         "invalid MLSD timestamps should remain absent");
}

static void common_list_formats_are_available_as_a_fallback() {
  const std::optional<FtpDirectoryEntry> unix_file =
      parse_ftp_list_entry(
          "-rw-r--r-- 1 alice staff 123 Jan 02 2024 report final.txt");
  const std::optional<FtpDirectoryEntry> unix_directory =
      parse_ftp_list_entry(
          "drwxr-xr-x 2 alice staff 4096 Feb 29 12:34 archive");
  const std::optional<FtpDirectoryEntry> dos_directory =
      parse_ftp_list_entry("02-29-24  12:34PM       <DIR>          uploads");
  const std::optional<FtpDirectoryEntry> dos_file =
      parse_ftp_list_entry("02-29-24  12:35PM                 987 note.txt");

  expect(unix_file.has_value() && unix_file->name == "report final.txt" &&
             unix_file->type == FtpDirectoryEntryType::regular &&
             unix_file->size == 123,
         "UNIX LIST regular file should be parsed");
  expect(unix_directory.has_value() &&
             unix_directory->type == FtpDirectoryEntryType::directory,
         "UNIX LIST directory should be parsed");
  expect(dos_directory.has_value() &&
             dos_directory->type == FtpDirectoryEntryType::directory,
         "DOS LIST directory should be parsed");
  expect(dos_file.has_value() && dos_file->size == 987,
         "DOS LIST regular file should be parsed");
  expect(!parse_ftp_list_entry("unrecognized listing").has_value(),
         "unknown LIST format should be ignored");
}

static void command_arguments_reject_control_line_injection() {
  expect(ftp_command_argument_is_safe("/reports/final.txt"),
         "ordinary FTP pathname should be safe");
  expect(!ftp_command_argument_is_safe("alice\r\nDELE /important"),
         "FTP command arguments must reject CRLF injection");
  expect(!ftp_command_argument_is_safe(std::string("a\0b", 3)),
         "FTP command arguments must reject NUL bytes");
}

} // namespace elder_terms

int main() {
  try {
    elder_terms::fragmented_and_pipelined_replies_are_parsed();
    elder_terms::multiline_reply_requires_the_matching_terminator();
    elder_terms::malformed_or_truncated_replies_are_rejected();
    elder_terms::passive_responses_are_validated();
    elder_terms::pwd_response_unescapes_embedded_quotes();
    elder_terms::mlsd_entries_preserve_machine_readable_facts();
    elder_terms::common_list_formats_are_available_as_a_fallback();
    elder_terms::command_arguments_reject_control_line_injection();
  } catch (const std::exception &exception) {
    std::cerr << "ftp-protocol-test: FAIL: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "ftp-protocol-test: PASS\n";
  return 0;
}
