#pragma once

#include <memory>
#include <optional>
#include <string>

struct DirectorySelection {
    std::string directory;
    std::string error;
};

class DirectoryPicker {
  public:
    DirectoryPicker(const std::string& directory, unsigned long parent);
    ~DirectoryPicker();
    int event_fd() const;
    std::optional<DirectorySelection> take_result();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
