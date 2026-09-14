#include "ftp-metadata.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>

namespace elder_terms {

static bool ascii_digit(char character) {
  return character >= '0' && character <= '9';
}

static std::string lower_ascii(std::string_view value) {
  std::string result(value);
  std::transform(
      result.begin(), result.end(), result.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return result;
}

template <typename Integer>
static std::optional<Integer> parse_unsigned_integer(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  Integer result = 0;
  const std::from_chars_result parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc() ||
      parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

std::string parse_ftp_pwd_path(std::string_view line) {
  if (!line.starts_with("257 ")) {
    throw std::runtime_error("Unexpected FTP PWD response code");
  }
  const std::size_t open = line.find('"');
  if (open == std::string_view::npos) {
    throw std::runtime_error("FTP PWD reply has no quoted pathname");
  }
  std::string path;
  for (std::size_t index = open + 1; index < line.size(); ++index) {
    if (line[index] != '"') {
      path.push_back(line[index]);
      continue;
    }
    if (index + 1 < line.size() && line[index + 1] == '"') {
      path.push_back('"');
      ++index;
      continue;
    }
    return path;
  }
  throw std::runtime_error("FTP PWD reply has an unterminated pathname");
}

static std::optional<std::int64_t>
parse_ftp_timestamp(std::string_view value) {
  if (value.size() < 14) {
    return std::nullopt;
  }
  if (!std::all_of(value.begin(), value.begin() + 14, ascii_digit)) {
    return std::nullopt;
  }
  if (value.size() > 14 &&
      (value[14] != '.' || value.size() == 15 ||
       !std::all_of(value.begin() + 15, value.end(), ascii_digit))) {
    return std::nullopt;
  }
  const auto decimal = [value](std::size_t offset, std::size_t count) {
    return parse_unsigned_integer<unsigned int>(value.substr(offset, count));
  };
  const std::optional<unsigned int> year = decimal(0, 4);
  const std::optional<unsigned int> month = decimal(4, 2);
  const std::optional<unsigned int> day = decimal(6, 2);
  const std::optional<unsigned int> hour = decimal(8, 2);
  const std::optional<unsigned int> minute = decimal(10, 2);
  const std::optional<unsigned int> second = decimal(12, 2);
  if (!year.has_value() || !month.has_value() || !day.has_value() ||
      !hour.has_value() || !minute.has_value() || !second.has_value() ||
      *hour > 23 || *minute > 59 || *second > 60) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{
      std::chrono::year(static_cast<int>(*year)),
      std::chrono::month(*month), std::chrono::day(*day)};
  if (!date.ok()) {
    return std::nullopt;
  }
  const std::chrono::sys_seconds time =
      std::chrono::sys_days(date) + std::chrono::hours(*hour) +
      std::chrono::minutes(*minute) +
      std::chrono::seconds(std::min(*second, 59U));
  return time.time_since_epoch().count();
}

std::optional<FtpDirectoryEntry>
parse_ftp_mlsd_entry(std::string_view line) {
  const std::size_t separator = line.find(' ');
  if (separator == std::string_view::npos || separator == 0 ||
      separator + 1 >= line.size()) {
    return std::nullopt;
  }
  const std::string_view facts = line.substr(0, separator);
  if (facts.back() != ';') {
    return std::nullopt;
  }
  FtpDirectoryEntry result{
      .name = std::string(line.substr(separator + 1)),
      .type = FtpDirectoryEntryType::other,
      .size = 0,
      .modification_time_unix_seconds = std::nullopt,
  };
  std::size_t begin = 0;
  while (begin < facts.size()) {
    const std::size_t end = facts.find(';', begin);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view fact = facts.substr(begin, end - begin);
    begin = end + 1;
    if (fact.empty()) {
      continue;
    }
    const std::size_t equals = fact.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      return std::nullopt;
    }
    const std::string key = lower_ascii(fact.substr(0, equals));
    const std::string_view value = fact.substr(equals + 1);
    if (key == "type") {
      const std::string type = lower_ascii(value);
      if (type == "file") {
        result.type = FtpDirectoryEntryType::regular;
      } else if (type == "dir") {
        result.type = FtpDirectoryEntryType::directory;
      } else if (type == "cdir") {
        result.type = FtpDirectoryEntryType::current_directory;
      } else if (type == "pdir") {
        result.type = FtpDirectoryEntryType::parent_directory;
      } else {
        result.type = FtpDirectoryEntryType::other;
      }
    } else if (key == "size") {
      result.size =
          parse_unsigned_integer<std::uint64_t>(value).value_or(0);
    } else if (key == "modify") {
      result.modification_time_unix_seconds = parse_ftp_timestamp(value);
    }
  }
  return result;
}

static std::optional<std::string_view>
take_space_delimited_token(std::string_view line, std::size_t *offset) {
  while (*offset < line.size() &&
         (line[*offset] == ' ' || line[*offset] == '\t')) {
    ++*offset;
  }
  if (*offset >= line.size()) {
    return std::nullopt;
  }
  const std::size_t begin = *offset;
  while (*offset < line.size() && line[*offset] != ' ' &&
         line[*offset] != '\t') {
    ++*offset;
  }
  return line.substr(begin, *offset - begin);
}

static std::string_view remaining_name(std::string_view line,
                                       std::size_t offset) {
  while (offset < line.size() &&
         (line[offset] == ' ' || line[offset] == '\t')) {
    ++offset;
  }
  return line.substr(offset);
}

static std::optional<FtpDirectoryEntry>
parse_unix_list_entry(std::string_view line) {
  std::size_t offset = 0;
  const std::optional<std::string_view> mode =
      take_space_delimited_token(line, &offset);
  if (!mode.has_value() || mode->size() < 10 ||
      (mode->front() != '-' && mode->front() != 'd' &&
       mode->front() != 'l')) {
    return std::nullopt;
  }
  const std::optional<std::string_view> links =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> owner =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> group =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> size_text =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> month =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> day =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> year_or_time =
      take_space_delimited_token(line, &offset);
  if (!links.has_value() || !owner.has_value() || !group.has_value() ||
      !size_text.has_value() || !month.has_value() || !day.has_value() ||
      !year_or_time.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> size =
      parse_unsigned_integer<std::uint64_t>(*size_text);
  std::string_view name = remaining_name(line, offset);
  if (!size.has_value() || name.empty()) {
    return std::nullopt;
  }
  FtpDirectoryEntryType type = FtpDirectoryEntryType::other;
  if (mode->front() == '-') {
    type = FtpDirectoryEntryType::regular;
  } else if (mode->front() == 'd') {
    type = FtpDirectoryEntryType::directory;
  } else {
    const std::size_t arrow = name.find(" -> ");
    if (arrow != std::string_view::npos) {
      name = name.substr(0, arrow);
    }
  }
  return FtpDirectoryEntry{
      .name = std::string(name),
      .type = type,
      .size = *size,
      .modification_time_unix_seconds = std::nullopt,
  };
}

static bool dos_date_token(std::string_view value) {
  return value.size() == 8 && value[2] == '-' && value[5] == '-' &&
         ascii_digit(value[0]) && ascii_digit(value[1]) &&
         ascii_digit(value[3]) && ascii_digit(value[4]) &&
         ascii_digit(value[6]) && ascii_digit(value[7]);
}

static bool dos_time_token(std::string_view value) {
  if (value.size() != 7 || value[2] != ':' ||
      !ascii_digit(value[0]) || !ascii_digit(value[1]) ||
      !ascii_digit(value[3]) || !ascii_digit(value[4])) {
    return false;
  }
  const std::string suffix = lower_ascii(value.substr(5));
  return suffix == "am" || suffix == "pm";
}

static std::optional<FtpDirectoryEntry>
parse_dos_list_entry(std::string_view line) {
  std::size_t offset = 0;
  const std::optional<std::string_view> date =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> time =
      take_space_delimited_token(line, &offset);
  const std::optional<std::string_view> size_or_directory =
      take_space_delimited_token(line, &offset);
  const std::string_view name = remaining_name(line, offset);
  if (!date.has_value() || !time.has_value() ||
      !size_or_directory.has_value() || !dos_date_token(*date) ||
      !dos_time_token(*time) || name.empty()) {
    return std::nullopt;
  }
  if (lower_ascii(*size_or_directory) == "<dir>") {
    return FtpDirectoryEntry{
        .name = std::string(name),
        .type = FtpDirectoryEntryType::directory,
        .size = 0,
        .modification_time_unix_seconds = std::nullopt,
    };
  }
  std::string size_text(*size_or_directory);
  size_text.erase(std::remove(size_text.begin(), size_text.end(), ','),
                  size_text.end());
  const std::optional<std::uint64_t> size =
      parse_unsigned_integer<std::uint64_t>(size_text);
  if (!size.has_value()) {
    return std::nullopt;
  }
  return FtpDirectoryEntry{
      .name = std::string(name),
      .type = FtpDirectoryEntryType::regular,
      .size = *size,
      .modification_time_unix_seconds = std::nullopt,
  };
}

std::optional<FtpDirectoryEntry>
parse_ftp_list_entry(std::string_view line) {
  const std::optional<FtpDirectoryEntry> unix =
      parse_unix_list_entry(line);
  return unix.has_value() ? unix : parse_dos_list_entry(line);
}

bool ftp_command_argument_is_safe(std::string_view value) {
  return std::all_of(
      value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f;
      });
}

} // namespace elder_terms
