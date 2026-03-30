#include "ui/home_menu.hpp"

#include <ftxui/dom/elements.hpp>

using namespace ftxui;

namespace forge::ui {

Element BuildHomeMenu(const std::vector<MenuItem>& items, int selected) {
  auto line = [](const std::string& text_value, Color c) {
    return text(text_value) | color(c) | bold | center;
  };

  Elements rows;
  
  rows.push_back(filler());
  rows.push_back(line(R"(  ______                    ____        _       _   )", Color::BlueLight));
  rows.push_back(line(R"( / ____/___  _________ ____/ __ )____  (_)___  / |_ )", Color::CyanLight));
  rows.push_back(line(R"(/ /_  / __ \/ ___/ __ `/ _  / __  / __ \/ / __ \/ __/)", Color::GreenLight));
  rows.push_back(line(R"(/ __/ / /_/ / /  / /_/ /  __/ /_/ / /_/ / / / / / /_ )", Color::YellowLight));
  rows.push_back(line(R"(/_/    \____/_/   \__, /\___/_____/\____/_/_/ /_/\__/ )", Color::MagentaLight));
  rows.push_back(line(R"(                 /____/                                  )", Color::RedLight));
  
  rows.push_back(separator());
  rows.push_back(text("What would you like to do?") | bold | center | color(Color::White));
  rows.push_back(text("Use ↑↓ to navigate, Enter to select") | center | dim);
  rows.push_back(separator());
  rows.push_back(text(""));

  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    bool is_selected = (static_cast<int>(i) == selected);
    
    Element row = hbox({
        text(is_selected ? "▸ " : "  ") | color(Color::CyanLight),
        text(item.label) | (is_selected ? bold : nothing),
    });
    
    if (is_selected) {
      row = vbox({
          row,
          text("  " + item.description) | dim | color(Color::GrayLight),
      });
    }
    
    if (is_selected) row = row | bgcolor(Color::GrayDark);
    rows.push_back(row);
    rows.push_back(text(""));
  }

  rows.push_back(separator());
  rows.push_back(text("Press q to quit · / for commands") | center | color(Color::CyanLight));
  rows.push_back(filler());

  return vbox(std::move(rows)) | border;
}

}  // namespace forge::ui
