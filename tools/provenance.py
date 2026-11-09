#!/usr/bin/env python3
"""Local JSONL service for intent records, reviewed Git snapshots, and line provenance."""

import json
import os
import sys

from provenance_store import Decisions


def write_message(message):
    data = memoryview((json.dumps(message, ensure_ascii=False) + "\n").encode())
    while data:
        data = data[os.write(sys.stdout.fileno(), data):]


class Service:
    def __init__(self):
        self.store = None

    def dispatch(self, method, params):
        if method == "initialize":
            self.store = Decisions(params["root"])
            self.store.repository.require_repository()
            return self.list_records()
        if not self.store:
            raise RuntimeError("Initialize the intent service first.")
        if method == "decisions/list":
            return self.list_records()
        if method == "decisions/record":
            self.store.record(params["decision"])
            return self.list_records()
        if method == "decisions/rationale":
            self.store.set_rationale(params["decision_id"], params["rationale"])
            return self.list_records()
        if method == "evidence/line":
            return self.store.inspect_line(params["path"], params["line"])
        if method == "evidence/commit":
            return self.store.inspect_commit(params["commit"], params.get("path"))
        if method == "evidence/preview":
            return self.store.preview(params["decision_id"], params.get("paths"), params.get("rationale"))
        if method == "evidence/checkpoint":
            result = self.store.checkpoint(params["token"])
            return {"evidence": result, **self.list_records()}
        raise RuntimeError("Unknown intent operation: " + method)

    def list_records(self):
        repository = self.store.repository
        return {"head": repository.head(), "decisions": self.store.records(),
                "working_paths": repository.working_paths()}


def main():
    service = Service()
    write_message({"method": "provenance/connected", "params": {}})
    for line in sys.stdin.buffer:
        request = None
        try:
            request = json.loads(line)
            result = service.dispatch(request["method"], request.get("params", {}))
            write_message({"id": request["id"], "result": result})
        except Exception as error:
            write_message({"id": request.get("id") if request else None,
                           "error": {"message": str(error)}})


if __name__ == "__main__":
    main()
