#include "ui/welcome_screen.hpp"

#include <ftxui/dom/elements.hpp>

using namespace ftxui;

namespace forge::ui {

Element BuildWelcomeScreen() {
  auto line = [](const std::string& text_value, Color c) {
    return text(text_value) | color(c) | bold | center;
  };

  return vbox({
             filler(),
             line(R"(  ______                    ____        _       _   )", Color::BlueLight),
             line(R"( / ____/___  _________ ____/ __ )____  (_)___  / |_ )", Color::CyanLight),
             line(R"(/ /_  / __ \/ ___/ __ `/ _  / __  / __ \/ / __ \/ __/)", Color::GreenLight),
             line(R"(/ __/ / /_/ / /  / /_/ /  __/ /_/ / /_/ / / / / / /_ )", Color::YellowLight),
             line(R"(/_/    \____/_/   \__, /\___/_____/\____/_/_/ /_/\__/ )", Color::MagentaLight),
             line(R"(                 /____/                                  )", Color::RedLight),
             separator(),
             text("Forge-Point") | bold | center | color(Color::White),
             text("A terminal cockpit for GGUF discovery, download, and llama.cpp server control.") | center | dim,
             separator(),
             paragraphAlignCenter("Press Enter to enter the cockpit.") |
                 size(WIDTH, LESS_THAN, 80),
             separator(),
             text("/ opens the command palette · Tab cycles panels · Enter acts") | center | color(Color::CyanLight),
             text("Ctrl+Y toggles YOLO mode · Esc returns to command palette") | center | dim,
             filler(),
         }) |
         border;
}

}  // namespace forge::ui
