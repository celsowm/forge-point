#include "core/string_utils.hpp"

#include <algorithm>
#include <iomanip>

namespace forge::util {

std::string Trim(std::string value) {
  auto is_not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
  return value;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool StartsWithIgnoreCase(const std::string& value, const std::string& prefix) {
  return ToLower(value).rfind(ToLower(prefix), 0) == 0;
}

std::string ReplaceAll(std::string value, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

std::string UrlEncode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex << std::uppercase;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      escaped << c;
    } else {
      escaped << '%' << std::setw(2) << int(c);
    }
  }
  return escaped.str();
}

std::string HumanBytes(std::uintmax_t bytes) {
  static const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  double count = static_cast<double>(bytes);
  size_t i = 0;
  while (count >= 1024.0 && i + 1 < std::size(suffixes)) {
    count /= 1024.0;
    ++i;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(i == 0 ? 0 : 1) << count << ' ' << suffixes[i];
  return oss.str();
}

std::vector<std::string> SplitArgs(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  bool in_quotes = false;
  char quote_char = '\0';
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if ((ch == '\'' || ch == '"') && (!in_quotes || quote_char == ch)) {
      if (in_quotes) {
        in_quotes = false;
        quote_char = '\0';
      } else {
        in_quotes = true;
        quote_char = ch;
      }
      continue;
    }
    if (!in_quotes && std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current += ch;
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

std::vector<std::string> SplitWords(const std::string& text) {
  std::istringstream iss(text);
  std::vector<std::string> out;
  std::string item;
  while (iss >> item) {
    out.push_back(item);
  }
  return out;
}

std::string ShellQuote(const std::string& arg) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"') out += '\\';
    out += c;
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : arg) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
#endif
}

std::string JoinCommand(const std::vector<std::string>& argv) {
  std::ostringstream oss;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) oss << ' ';
    oss << ShellQuote(argv[i]);
  }
  return oss.str();
}

}  // namespace forge::util
