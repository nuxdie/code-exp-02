#pragma once

#include <memory>
#include <string>
#include <vector>

struct TypesetMath {
    std::string key;
    std::string svg;
    std::string error;
};

// TeX runs on a worker; the UI receives cached vector artwork through an event fd.
class MathTypesetter {
  public:
    MathTypesetter();
    ~MathTypesetter();
    void request(const std::string& expression, bool display);
    int event_fd() const;
    std::vector<TypesetMath> take_results();
    static std::string key(const std::string& expression, bool display);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
