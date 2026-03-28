#include "app/app.hpp"

#include <cstdio>
#include <exception>

int main() {
  try {
    forge::App app;
    app.Run();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "Forge-Point crashed: %s\n", ex.what());
    return 1;
  }
}
