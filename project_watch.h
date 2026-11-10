#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using ProjectSnapshot = std::map<std::string, std::string>;
struct ProjectChange {
    std::string path;
    std::optional<std::string> source;
};

bool project_image(const std::string& path);
bool project_source(const std::string& path);
ProjectSnapshot read_project(const std::string& root);
std::vector<ProjectChange> project_changes(const ProjectSnapshot& before,
                                           const ProjectSnapshot& after);

// Watches directories as well as files so editor saves by rename are observed.
class ProjectWatcher {
  public:
    ProjectWatcher(std::string root, ProjectSnapshot initial);
    ~ProjectWatcher();
    int event_fd() const;
    std::vector<ProjectChange> take_changes();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
