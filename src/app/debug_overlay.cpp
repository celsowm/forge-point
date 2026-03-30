#include "app/debug_overlay.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace forge {

DebugOverlay& DebugOverlay::Instance() {
  static DebugOverlay instance;
  return instance;
}

void DebugOverlay::Log(LogLevel level, const std::string& message, const std::string& source) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  LogEntry entry;
  entry.message = message;
  entry.level = level;
  entry.timestamp = std::chrono::system_clock::now();
  entry.source = source;
  
  entries_.push_back(entry);
  
  // Keep only last MAX_ENTRIES
  if (entries_.size() > MAX_ENTRIES) {
    entries_.erase(entries_.begin());
  }
}

void DebugOverlay::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

Element DebugOverlay::Render() const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (!visible_ || entries_.empty()) {
    return text("");
  }
  
  Elements rows;
  rows.push_back(text("=== DEBUG LOG (Press F12 to toggle) ===") | bold | color(Color::CyanLight));
  rows.push_back(separator());
  
  // Show last 20 entries
  size_t start = entries_.size() > 20 ? entries_.size() - 20 : 0;
  
  for (size_t i = start; i < entries_.size(); ++i) {
    const auto& entry = entries_[i];
    
    // Format timestamp
    auto time = std::chrono::system_clock::to_time_t(entry.timestamp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%H:%M:%S");
    
    // Color by level
    Color level_color = Color::White;
    std::string level_str = "???";
    switch (entry.level) {
      case LogLevel::Debug: level_color = Color::GrayLight; level_str = "DBG"; break;
      case LogLevel::Info: level_color = Color::GreenLight; level_str = "INF"; break;
      case LogLevel::Warning: level_color = Color::YellowLight; level_str = "WRN"; break;
      case LogLevel::Error: level_color = Color::RedLight; level_str = "ERR"; break;
    }
    
    std::string line = "[" + ss.str() + "] [" + level_str + "] ";
    if (!entry.source.empty()) {
      line += entry.source + ": ";
    }
    line += entry.message;
    
    rows.push_back(text(line) | color(level_color));
  }
  
  return vbox(std::move(rows)) | border | bgcolor(Color::Black);
}

}  // namespace forge
