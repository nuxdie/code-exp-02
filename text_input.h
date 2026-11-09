#pragma once

#include <cstddef>
#include <string>
#include <utility>

class TextInput {
  public:
    std::string text;
    std::size_t cursor = 0, anchor = 0;
    void set(std::string value);
    void insert(const std::string& value);
    void erase(bool backwards);
    void move(int direction, bool extend);
    void line_end(bool end, bool extend);
    void select_all();
    std::pair<std::size_t, std::size_t> selection() const;
    std::string selected() const;
    void place(std::size_t byte, bool extend = false);

  private:
    std::size_t previous(std::size_t byte) const;
    std::size_t next(std::size_t byte) const;
};
