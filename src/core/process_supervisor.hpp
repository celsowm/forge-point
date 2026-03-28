#pragma once

#include "interfaces.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace forge {

class ProcessSupervisor : public IProcess {
 public:
  ProcessSupervisor();
  ~ProcessSupervisor() override;

  bool Start(const std::vector<std::string>& argv, LogFn log_fn) override;
  void Stop() override;
  bool Running() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> running_{false};
  LogFn log_fn_;
  std::thread reader_thread_;
};

}  // namespace forge
