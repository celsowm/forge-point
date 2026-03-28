#include "app/logger.hpp"

#include <sstream>

namespace forge {

Logger::Logger(size_t max_logs) : max_logs_(max_logs) {}

void Logger::Log(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::istringstream iss(message);
  std::string part;
  while (std::getline(iss, part)) {
    logs_.push_back(part);
  }
  while (logs_.size() > max_logs_) {
    logs_.erase(logs_.begin());
  }
}

std::vector<std::string> Logger::GetLogs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return logs_;
}

size_t Logger::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return logs_.size();
}

}  // namespace forge
