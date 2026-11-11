#pragma once

#include "lsp.h"

#include <deque>
#include <map>
#include <set>

struct SymbolQuery {
    std::string uri;
    int version = 0;
    int line = 0;
    std::size_t begin = 0, end = 0;
    bool local = true;
    unsigned long generation = 0;
};

struct SymbolConnection {
    SymbolQuery source;
    std::string target_uri;
    int line = 0, column = 0;
};

// Resolve semantic uses in small batches, independently of interactive navigation requests.
class SymbolGraph {
  public:
    void reset(std::set<std::string> documents);
    void update(const std::string& uri, int version, const std::vector<lsp::SemanticToken>& tokens,
                unsigned declarations);
    std::optional<SymbolQuery> next();
    void started(int id, const SymbolQuery& query);
    bool complete(const lsp::Message& message);
    bool idle() const { return queued.empty() && pending.empty(); }
    const std::vector<SymbolConnection>& connections() const { return edges; }

  private:
    std::set<std::string> documents;
    std::map<std::string, unsigned long> generations;
    unsigned long generation = 0;
    std::deque<SymbolQuery> queued;
    std::map<int, SymbolQuery> pending;
    std::vector<SymbolConnection> edges;
    void connect(const SymbolQuery& source, const lsp::Json& location);
};
