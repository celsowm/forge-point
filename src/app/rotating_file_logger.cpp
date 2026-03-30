#include "app/rotating_file_logger.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace forge {

RotatingFileLogger::RotatingFileLogger(const fs::path& log_dir,
                                       const std::string& base_name,
                                       size_t max_file_size,
                                       size_t max_files)
    : log_dir_(log_dir),
      base_name_(base_name),
      max_file_size_(max_file_size),
      max_files_(max_files) {
  std::error_code ec;
  fs::create_directories(log_dir_, ec);

  current_path_ = log_dir_ / (base_name_ + ".log");
  file_.open(current_path_, std::ios::app);
  if (file_) {
    current_size_ = static_cast<size_t>(fs::file_size(current_path_, ec));
  }
}

void RotatingFileLogger::Write(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_.is_open()) return;

  std::string line = "[" + Timestamp() + "] " + message + "\n";
  file_ << line;
  file_.flush();
  current_size_ += line.size();

  if (current_size_ >= max_file_size_) {
    Rotate();
  }
}

fs::path RotatingFileLogger::CurrentLogPath() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_path_;
}

void RotatingFileLogger::Rotate() {
  file_.close();

  // Shift existing rotated files: .4.log -> .5.log (delete), .3.log -> .4.log, etc.
  for (size_t i = max_files_; i > 0; --i) {
    fs::path old = log_dir_ / (base_name_ + "." + std::to_string(i) + ".log");
    fs::path older = log_dir_ / (base_name_ + "." + std::to_string(i - 1) + ".log");
    if (i == max_files_) {
      std::error_code ec;
      fs::remove(old, ec);
    }
    if (i > 1) {
      std::error_code ec;
      fs::rename(older, old, ec);
    }
  }

  // Rename current -> .1.log
  fs::path first_rotated = log_dir_ / (base_name_ + ".1.log");
  std::error_code ec;
  fs::rename(current_path_, first_rotated, ec);

  // Open fresh file
  file_.open(current_path_, std::ios::trunc);
  current_size_ = 0;
}

std::string RotatingFileLogger::Timestamp() const {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()) % 1000;

  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
      << "." << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

}  // namespace forge
