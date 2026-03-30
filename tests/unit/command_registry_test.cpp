#include <gtest/gtest.h>
#include "app/command_registry.hpp"

namespace forge {

TEST(CommandRegistryTest, GetAllReturnsCommands) {
  CommandRegistry registry;
  
  auto commands = registry.GetAll();
  
  EXPECT_GT(commands.size(), 0);
}

TEST(CommandRegistryTest, MatchExactCommand) {
  CommandRegistry registry;
  
  auto matches = registry.Match("/help");
  
  EXPECT_GT(matches.size(), 0);
  EXPECT_EQ(matches[0].name, "/help");
}

TEST(CommandRegistryTest, MatchPartialCommand) {
  CommandRegistry registry;
  
  auto matches = registry.Match("/he");
  
  EXPECT_GT(matches.size(), 0);
  for (const auto& cmd : matches) {
    EXPECT_TRUE(cmd.name.find("/he") == 0);
  }
}

TEST(CommandRegistryTest, MatchNoResults) {
  CommandRegistry registry;
  
  auto matches = registry.Match("/xyznonexistent");
  
  EXPECT_EQ(matches.size(), 0);
}

TEST(CommandRegistryTest, MatchIsCaseInsensitive) {
  CommandRegistry registry;
  
  auto matches = registry.Match("/HELP");
  
  // Commands are case-insensitive
  EXPECT_GT(matches.size(), 0);
  EXPECT_EQ(matches[0].name, "/help");
}

TEST(CommandRegistryTest, MatchWithArgs) {
  CommandRegistry registry;
  
  auto matches = registry.Match("/search qwen");
  
  EXPECT_GT(matches.size(), 0);
  EXPECT_EQ(matches[0].name, "/search");
}

}  // namespace forge
