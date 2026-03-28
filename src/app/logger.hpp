#pragma once

#include "interfaces.hpp"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace forge {

class Logger : public ILogger {
 public:
  explicit Logger(size_t max_logs = 400);

  void Log(const std::string& message) override;
  std::vector<std::string> GetLogs() const;
  size_t Size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> logs_;
  size_t max_logs_;
};

}  // namespace forge
