#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace forge {

class RotatingFileLogger {
 public:
  RotatingFileLogger(const fs::path& log_dir,
                     const std::string& base_name = "forge-point",
                     size_t max_file_size = 5 * 1024 * 1024,  // 5 MB
                     size_t max_files = 5);

  void Write(const std::string& message);
  fs::path CurrentLogPath() const;

 private:
  void Rotate();
  std::string Timestamp() const;

  mutable std::mutex mutex_;
  fs::path log_dir_;
  std::string base_name_;
  fs::path current_path_;
  std::ofstream file_;
  size_t max_file_size_;
  size_t max_files_;
  size_t current_size_ = 0;
};

}  // namespace forge
