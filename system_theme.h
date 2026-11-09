#pragma once
#include <memory>
#include <optional>
#include <string>

bool theme_name_is_dark(const std::string& name);
std::optional<bool> preferred_dark_theme(unsigned preference);

class SystemTheme {
  public:
    SystemTheme();
    ~SystemTheme();
    int event_fd() const;
    std::optional<bool> take_change();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
