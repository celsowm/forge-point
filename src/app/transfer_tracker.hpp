#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace forge {

struct Transfer {
  std::string label;
  uint64_t downloaded = 0;
  uint64_t total = 0;
  bool done = false;
  bool failed = false;
};

class TransferTracker {
 public:
  size_t Add(const std::string& label);
  void Update(size_t idx, uint64_t downloaded, uint64_t total);
  void Finish(size_t idx, bool failed = false);
  void CleanupDone();
  std::vector<Transfer> GetActive() const;
  std::vector<Transfer> GetAll() const;

 private:
  mutable std::mutex mutex_;
  std::vector<Transfer> transfers_;
};

}  // namespace forge
