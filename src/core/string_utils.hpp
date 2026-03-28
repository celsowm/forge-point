#pragma once

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace forge::util {

std::string Trim(std::string value);
std::string ToLower(std::string value);
bool StartsWithIgnoreCase(const std::string& value, const std::string& prefix);
std::string ReplaceAll(std::string value, const std::string& from, const std::string& to);
std::string UrlEncode(const std::string& value);
std::string HumanBytes(std::uintmax_t bytes);
std::vector<std::string> SplitArgs(const std::string& text);
std::vector<std::string> SplitWords(const std::string& text);
std::string ShellQuote(const std::string& arg);
std::string JoinCommand(const std::vector<std::string>& argv);

}  // namespace forge::util
