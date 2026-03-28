#include "app/confirmation_guard.hpp"

namespace forge {

void ConfirmationGuard::Request(std::string title, std::string detail,
                                 bool guarded, std::function<void()> action) {
  if (guarded && !yolo_mode_) {
    title_ = std::move(title);
    detail_ = std::move(detail);
    pending_action_ = std::move(action);
    show_ = true;
    return;
  }
  action();
}

bool ConfirmationGuard::Confirm() {
  if (!show_ || !pending_action_) return false;
  show_ = false;
  auto action = std::move(pending_action_);
  pending_action_ = nullptr;
  action();
  return true;
}

void ConfirmationGuard::Cancel() {
  if (!show_) return;
  show_ = false;
  pending_action_ = nullptr;
}

}  // namespace forge
