#pragma once

#include "interfaces.hpp"
#include "app/rotating_file_logger.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace forge {

class Logger : public ILogger {
 public:
  explicit Logger(size_t max_logs = 400);

  void SetFileLogger(std::unique_ptr<RotatingFileLogger> file_logger);

  void Log(const std::string& message) override;
  std::vector<std::string> GetLogs() const;
  size_t Size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> logs_;
  size_t max_logs_;
  std::unique_ptr<RotatingFileLogger> file_logger_;
};

}  // namespace forge
