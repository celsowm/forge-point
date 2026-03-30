#include <gtest/gtest.h>
#include "app/command_handler.hpp"

namespace forge {

TEST(CommandHandlerTest, RegisterAndExecute) {
  CommandHandler handler;
  bool executed = false;
  
  handler.Register("/test", [&executed](const std::vector<std::string>&) {
    executed = true;
  });
  
  bool result = handler.Execute("/test", {});
  
  EXPECT_TRUE(result);
  EXPECT_TRUE(executed);
}

TEST(CommandHandlerTest, ExecuteUnknownCommand) {
  CommandHandler handler;
  
  bool result = handler.Execute("/unknown", {});
  
  EXPECT_FALSE(result);
}

TEST(CommandHandlerTest, ExecuteWithArgs) {
  CommandHandler handler;
  std::vector<std::string> received_args;
  
  handler.Register("/echo", [&received_args](const std::vector<std::string>& args) {
    received_args = args;
  });
  
  handler.Execute("/echo", {"hello", "world"});
  
  ASSERT_EQ(received_args.size(), 2);
  EXPECT_EQ(received_args[0], "hello");
  EXPECT_EQ(received_args[1], "world");
}

TEST(CommandHandlerTest, CaseInsensitiveCommand) {
  CommandHandler handler;
  bool executed = false;
  
  handler.Register("/test", [&executed](const auto&) { executed = true; });
  
  handler.Execute("/TEST", {});
  
  EXPECT_FALSE(executed);  // Commands are case-sensitive by default
}

}  // namespace forge
