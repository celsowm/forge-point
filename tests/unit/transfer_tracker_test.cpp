#include <gtest/gtest.h>
#include "app/transfer_tracker.hpp"

namespace forge {

TEST(TransferTrackerTest, AddTransfer) {
  TransferTracker tracker;
  
  size_t id = tracker.Add("test-file.zip");
  
  auto active = tracker.GetActive();
  ASSERT_EQ(active.size(), 1);
  EXPECT_EQ(active[0].label, "test-file.zip");
  EXPECT_EQ(active[0].downloaded, 0);
  EXPECT_EQ(active[0].total, 0);
}

TEST(TransferTrackerTest, UpdateTransfer) {
  TransferTracker tracker;
  
  size_t id = tracker.Add("test-file.zip");
  tracker.Update(id, 500, 1000);
  
  auto active = tracker.GetActive();
  ASSERT_EQ(active.size(), 1);
  EXPECT_EQ(active[0].downloaded, 500);
  EXPECT_EQ(active[0].total, 1000);
}

TEST(TransferTrackerTest, FinishTransfer) {
  TransferTracker tracker;
  
  size_t id = tracker.Add("test-file.zip");
  tracker.Finish(id);
  
  auto active = tracker.GetActive();
  EXPECT_EQ(active.size(), 0);
}

TEST(TransferTrackerTest, FinishWithError) {
  TransferTracker tracker;
  
  size_t id = tracker.Add("test-file.zip");
  tracker.Finish(id, true);  // true = error
  
  auto active = tracker.GetActive();
  EXPECT_EQ(active.size(), 0);
}

TEST(TransferTrackerTest, MultipleTransfers) {
  TransferTracker tracker;
  
  size_t id1 = tracker.Add("file1.zip");
  size_t id2 = tracker.Add("file2.zip");
  size_t id3 = tracker.Add("file3.zip");
  
  auto active = tracker.GetActive();
  ASSERT_EQ(active.size(), 3);
  
  tracker.Finish(id2);
  active = tracker.GetActive();
  ASSERT_EQ(active.size(), 2);
  
  tracker.Finish(id1);
  tracker.Finish(id3);
  active = tracker.GetActive();
  ASSERT_EQ(active.size(), 0);
}

}  // namespace forge
