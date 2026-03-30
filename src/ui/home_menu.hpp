#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

using namespace ftxui;

namespace forge::ui {

struct MenuItem {
  std::string label;
  std::string description;
  std::string shortcut;
};

Element BuildHomeMenu(const std::vector<MenuItem>& items, int selected);

}  // namespace forge::ui
