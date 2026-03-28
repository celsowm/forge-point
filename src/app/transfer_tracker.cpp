#include "app/transfer_tracker.hpp"

#include <algorithm>

namespace forge {

size_t TransferTracker::Add(const std::string& label) {
  std::lock_guard<std::mutex> lock(mutex_);
  transfers_.push_back({label, 0, 0, false, false});
  return transfers_.size() - 1;
}

void TransferTracker::Update(size_t idx, uint64_t downloaded, uint64_t total) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (idx < transfers_.size()) {
    transfers_[idx].downloaded = downloaded;
    transfers_[idx].total = total;
  }
}

void TransferTracker::Finish(size_t idx, bool failed) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (idx < transfers_.size()) {
    transfers_[idx].done = true;
    transfers_[idx].failed = failed;
  }
}

void TransferTracker::CleanupDone() {
  std::lock_guard<std::mutex> lock(mutex_);
  transfers_.erase(
      std::remove_if(transfers_.begin(), transfers_.end(),
                     [](const Transfer& t) { return t.done; }),
      transfers_.end());
}

std::vector<Transfer> TransferTracker::GetActive() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Transfer> active;
  for (const auto& t : transfers_) {
    if (!t.done) active.push_back(t);
  }
  return active;
}

std::vector<Transfer> TransferTracker::GetAll() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return transfers_;
}

}  // namespace forge
