#include <gtest/gtest.h>
#include "app/confirmation_guard.hpp"

namespace forge {

TEST(ConfirmationGuardTest, YoloModeExecutesImmediately) {
  ConfirmationGuard guard;
  guard.SetYolo(true);
  
  bool executed = false;
  guard.Request("Title", "Detail", true, [&executed] { executed = true; });
  
  EXPECT_TRUE(executed);
  EXPECT_FALSE(guard.IsShowing());
}

TEST(ConfirmationGuardTest, GuardedModeShowsConfirmation) {
  ConfirmationGuard guard;
  guard.SetYolo(false);
  
  bool executed = false;
  guard.Request("Download", "Download file?", true, [&executed] { executed = true; });
  
  EXPECT_FALSE(executed);
  EXPECT_TRUE(guard.IsShowing());
}

TEST(ConfirmationGuardTest, ConfirmExecutesAction) {
  ConfirmationGuard guard;
  guard.SetYolo(false);
  
  bool executed = false;
  guard.Request("Test", "Test action", true, [&executed] { executed = true; });
  
  guard.Confirm();
  
  EXPECT_TRUE(executed);
  EXPECT_FALSE(guard.IsShowing());
}

TEST(ConfirmationGuardTest, CancelDoesNotExecuteAction) {
  ConfirmationGuard guard;
  guard.SetYolo(false);
  
  bool executed = false;
  guard.Request("Test", "Test action", true, [&executed] { executed = true; });
  
  guard.Cancel();
  
  EXPECT_FALSE(executed);
  EXPECT_FALSE(guard.IsShowing());
}

TEST(ConfirmationGuardTest, ToggleYolo) {
  ConfirmationGuard guard;
  
  EXPECT_FALSE(guard.YoloMode());
  
  guard.ToggleYolo();
  EXPECT_TRUE(guard.YoloMode());
  
  guard.ToggleYolo();
  EXPECT_FALSE(guard.YoloMode());
}

TEST(ConfirmationGuardTest, UnguardedActionExecutesImmediately) {
  ConfirmationGuard guard;
  
  bool executed = false;
  guard.Request("Test", "Test", false, [&executed] { executed = true; });
  
  EXPECT_TRUE(executed);
  EXPECT_FALSE(guard.IsShowing());
}

}  // namespace forge
