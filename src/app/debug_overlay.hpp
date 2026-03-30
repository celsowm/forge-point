#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>

using namespace ftxui;

namespace forge {

enum class LogLevel {
  Debug,
  Info,
  Warning,
  Error
};

struct LogEntry {
  std::string message;
  LogLevel level;
  std::chrono::system_clock::time_point timestamp;
  std::string source;
};

class DebugOverlay {
 public:
  static DebugOverlay& Instance();
  
  void Log(LogLevel level, const std::string& message, const std::string& source = "");
  void Debug(const std::string& msg, const std::string& src = "") {
    Log(LogLevel::Debug, msg, src);
  }
  void Info(const std::string& msg, const std::string& src = "") {
    Log(LogLevel::Info, msg, src);
  }
  void Warning(const std::string& msg, const std::string& src = "") {
    Log(LogLevel::Warning, msg, src);
  }
  void Error(const std::string& msg, const std::string& src = "") {
    Log(LogLevel::Error, msg, src);
  }
  
  void SetVisible(bool visible) { visible_ = visible; }
  bool IsVisible() const { return visible_; }
  void Toggle() { visible_ = !visible_; }
  void Clear();
  
  Element Render() const;
  const std::vector<LogEntry>& GetEntries() const { return entries_; }
  
 private:
  DebugOverlay() = default;
  
  mutable std::mutex mutex_;
  std::vector<LogEntry> entries_;
  bool visible_ = false;
  static constexpr size_t MAX_ENTRIES = 100;
};

}  // namespace forge
