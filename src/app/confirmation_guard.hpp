#pragma once

#include <functional>
#include <string>

namespace forge {

class ConfirmationGuard {
 public:
  void Request(std::string title, std::string detail, bool guarded,
               std::function<void()> action);
  bool Confirm();
  void Cancel();

  bool IsShowing() const { return show_; }
  bool YoloMode() const { return yolo_mode_; }
  void SetYolo(bool enabled) { yolo_mode_ = enabled; }
  void ToggleYolo() { yolo_mode_ = !yolo_mode_; }

  const std::string& Title() const { return title_; }
  const std::string& Detail() const { return detail_; }

 private:
  bool yolo_mode_ = false;
  bool show_ = false;
  std::string title_;
  std::string detail_;
  std::function<void()> pending_action_;
};

}  // namespace forge
