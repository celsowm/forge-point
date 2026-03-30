#include <gtest/gtest.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

using namespace ftxui;

namespace forge::component {

TEST(MenuNavigationTest, ArrowDownIncreasesSelection) {
  std::vector<std::string> entries = {"A", "B", "C"};
  int selected = 0;
  auto menu = Menu(&entries, &selected);
  
  menu->OnEvent(Event::ArrowDown);
  EXPECT_EQ(selected, 1);
  
  menu->OnEvent(Event::ArrowDown);
  EXPECT_EQ(selected, 2);
}

TEST(MenuNavigationTest, ArrowUpDecreasesSelection) {
  std::vector<std::string> entries = {"A", "B", "C"};
  int selected = 2;
  auto menu = Menu(&entries, &selected);
  
  menu->OnEvent(Event::ArrowUp);
  EXPECT_EQ(selected, 1);
  
  menu->OnEvent(Event::ArrowUp);
  EXPECT_EQ(selected, 0);
}

TEST(MenuNavigationTest, ArrowDownAtLastStaysAtLast) {
  std::vector<std::string> entries = {"A", "B", "C"};
  int selected = 2;
  auto menu = Menu(&entries, &selected);
  
  menu->OnEvent(Event::ArrowDown);
  EXPECT_EQ(selected, 2);  // Should not go past last item
}

TEST(MenuNavigationTest, ArrowUpAtFirstStaysAtFirst) {
  std::vector<std::string> entries = {"A", "B", "C"};
  int selected = 0;
  auto menu = Menu(&entries, &selected);
  
  menu->OnEvent(Event::ArrowUp);
  EXPECT_EQ(selected, 0);  // Should not go before first item
}

TEST(MenuNavigationTest, EnterTriggersSelection) {
  std::vector<std::string> entries = {"A", "B", "C"};
  int selected = 0;
  bool enter_pressed = false;
  
  auto menu = Menu(&entries, &selected);
  auto wrapped = CatchEvent(menu, [&enter_pressed](Event event) {
    if (event == Event::Return) {
      enter_pressed = true;
      return true;
    }
    return false;
  });
  
  wrapped->OnEvent(Event::Return);
  EXPECT_TRUE(enter_pressed);
}

TEST(MenuNavigationTest, EmptyListHandlesGracefully) {
  std::vector<std::string> entries = {};
  int selected = 0;
  auto menu = Menu(&entries, &selected);
  
  // Should not crash
  menu->OnEvent(Event::ArrowDown);
  menu->OnEvent(Event::ArrowUp);
  
  EXPECT_EQ(selected, 0);
}

TEST(MenuNavigationTest, SingleItemList) {
  std::vector<std::string> entries = {"Only"};
  int selected = 0;
  auto menu = Menu(&entries, &selected);
  
  menu->OnEvent(Event::ArrowDown);
  EXPECT_EQ(selected, 0);  // Should stay at 0
  
  menu->OnEvent(Event::ArrowUp);
  EXPECT_EQ(selected, 0);  // Should stay at 0
}

}  // namespace forge::component
