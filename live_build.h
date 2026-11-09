#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct BuildResult {
    uint64_t generation = 0;
    bool started = false;
    bool success = false;
    std::string output;
};
class LiveBuild {
  public:
    explicit LiveBuild(std::string directory);
    ~LiveBuild();
    void request(uint64_t generation);
    int event_fd() const;
    std::vector<BuildResult> take_results();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
