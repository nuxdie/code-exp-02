"""Durable human intent and explicit rationale, linked through Git commit trailers."""

import copy
import datetime
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import uuid

from provenance_git import GitError, Repository


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class Decisions:
    def __init__(self, root):
        self.repository = Repository(root)
        self.path = self.repository.root / ".canvas/decisions.json"
        self.previews = {}

    def load(self):
        if not self.path.exists():
            return {"schema_version": 1, "decisions": []}
        document = json.loads(self.path.read_text())
        if document.get("schema_version") != 1 or not isinstance(document.get("decisions"), list):
            raise GitError("Unsupported decision document.")
        for decision in document["decisions"]:
            if decision.get("author", {}).get("role") != "human":
                raise GitError("A decision must identify its human source.")
        return document

    def save(self, document):
        self.path.parent.mkdir(exist_ok=True)
        temporary = self.path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        os.replace(temporary, self.path)

    def record(self, record):
        document = self.load()
        statement = record.get("statement", "")
        if not isinstance(statement, str) or not statement.strip():
            raise GitError("Capture the human's exact statement before linking an artifact.")
        existing = next((d for d in document["decisions"] if d["id"] == record["id"]), None)
        if existing:
            if existing["statement"] != statement or existing.get("origin") != record.get("origin"):
                raise GitError("A recorded human statement is immutable; record a new decision to revise it.")
            return existing
        record = copy.deepcopy(record)
        record["author"] = {"role": "human", "label": "User"}
        record["recorded_at"] = utc_now()
        record["statement_hash"] = hashlib.sha256(statement.encode()).hexdigest()
        record.setdefault("artifacts", [])
        record.setdefault("relations", [])
        record.setdefault("rationale", {"summary": "", "author_role": "agent", "basis": "not_recorded"})
        record.setdefault("association", {"kind": "captured_statement", "precision": "unbound"})
        document["decisions"].append(record)
        self.save(document)
        return record

    def set_rationale(self, decision_id, rationale):
        document = self.load()
        decision = next((d for d in document["decisions"] if d["id"] == decision_id), None)
        if not decision:
            raise GitError("Select a recorded human decision first.")
        if not rationale.get("summary", "").strip() or rationale.get("author_role") not in {"human", "agent"}:
            raise GitError("A rationale needs a summary and its actual author role.")
        decision.setdefault("rationale_history", []).append(decision.get("rationale", {}))
        decision["rationale"] = {**rationale, "recorded_at": utc_now()}
        self.save(document)

    def records(self):
        records = copy.deepcopy(self.load()["decisions"])
        commits = self.repository.commits()
        for record in records:
            record["commits"] = [c for c in commits if record["id"] in c["intent_ids"]]
        return records

    def resolve(self, revision, path=None):
        commit = self.repository.commit(revision)
        ids = commit["intent_ids"]
        historical = json.loads(self.repository.git("show", commit["commit"] + ":.canvas/decisions.json", check=False) or '{"decisions":[]}')
        records = [copy.deepcopy(record) for record in historical["decisions"] if record["id"] in ids and
                   (not path or any(fnmatch.fnmatchcase(path, pattern) for pattern in record.get("artifacts", [])))]
        for record in records:
            record["binding"] = {"commit": commit["commit"], "kind": "reconstructed_milestone" if commit["reconstructed"] else "recorded_commit"}
        return records

    def inspect_line(self, path, line):
        evidence = self.repository.line(path, int(line))
        evidence["decisions"] = self.resolve(evidence["commit"], evidence.get("origin_path", evidence["path"])) if evidence["commit"] else []
        return evidence

    def inspect_commit(self, revision, path=None):
        commit = self.repository.commit(revision)
        return {**commit, "kind": "commit", "decisions": self.resolve(revision, path),
                "changes": self.repository.changes(revision), "path": path,
                "patch": self.repository.diff(revision, path)}

    def preview(self, decision_id, paths=None, rationale=None):
        self.repository.require_repository()
        document = self.load()
        decision = next((d for d in document["decisions"] if d["id"] == decision_id), None)
        if not decision:
            raise GitError("Select a recorded human decision first.")
        if rationale:
            summary = rationale.get("summary", "").strip()
            if not summary or rationale.get("author_role") not in {"human", "agent"}:
                raise GitError("A rationale needs a summary and its actual author role.")
            if rationale != decision.get("rationale"):
                decision.setdefault("rationale_history", []).append(decision.get("rationale", {}))
                decision["rationale"] = {**rationale, "recorded_at": utc_now()}
        changed = self.repository.working_paths()
        selected = [self.repository.path(p) for p in (paths if paths is not None else changed)]
        if any(path not in changed for path in selected):
            raise GitError("Select changed artifact files for this checkpoint.")
        if selected and not decision.get("rationale", {}).get("summary", "").strip():
            raise GitError("Record an explicit rationale before checkpointing artifact changes.")
        decision["artifacts"] = sorted(set(decision.get("artifacts", []) + selected))
        decision["association"] = {"kind": "reviewed_checkpoint", "precision": "selected_diff"}
        self.save(document)
        snapshot = self.repository.snapshot(selected + [".canvas/decisions.json"])
        if snapshot["base"] and snapshot["tree"] == self.repository.git("rev-parse", snapshot["base"] + "^{tree}").strip():
            raise GitError("The selected decision and artifacts have no changes to checkpoint.")
        token = uuid.uuid4().hex
        value = {**snapshot, "token": token, "decision_id": decision_id, "paths": selected,
                 "statement": decision["statement"], "rationale": decision["rationale"]}
        self.previews[token] = copy.deepcopy(value)
        return value

    def checkpoint(self, token):
        preview = self.previews.get(token)
        if not preview:
            raise GitError("Preview the exact diff before checkpointing it.")
        repository = self.repository
        if repository.head() != preview["base"]:
            raise GitError("Git HEAD moved after this preview. Refresh the diff before checkpointing.")
        index_path = Path(repository.git("rev-parse", "--git-path", "index").strip())
        if not index_path.is_absolute():
            index_path = repository.root / index_path
        lock = Path(str(index_path) + ".lock")
        try:
            descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        except FileExistsError as error:
            raise GitError("Another Git operation currently owns the index. Try the checkpoint again when it finishes.") from error
        os.close(descriptor)
        try:
            return self._commit_snapshot(preview, index_path)
        finally:
            lock.unlink(missing_ok=True)

    def _commit_snapshot(self, preview, index_path):
        repository = self.repository
        paths = preview["paths"] + [".canvas/decisions.json"]
        with tempfile.TemporaryDirectory(prefix="intent-commit-", dir=repository.root / ".cache") as directory:
            next_index = Path(directory) / "index"
            environment = dict(os.environ, GIT_INDEX_FILE=str(next_index))
            if index_path.exists():
                shutil.copyfile(index_path, next_index)
            else:
                repository.git("read-tree", "--empty", environment=environment)
            # Replace only reviewed paths; unrelated staged entries retain their exact blobs.
            entries = b"".join((f"0 {'0' * 40}\t{path}\0").encode() for path in paths)
            for entry in repository.git("ls-tree", "-r", "-z", preview["tree"], "--", *paths).split("\0"):
                if entry:
                    metadata, path = entry.split("\t", 1)
                    mode, kind, blob = metadata.split()
                    entries += f"{mode} {blob}\t{path}\0".encode()
            repository.git("update-index", "-z", "--index-info", data=entries, environment=environment)
            message = ("Checkpoint human intent: " + preview["statement"].splitlines()[0][:80] +
                       "\n\nHuman intent:\n" + preview["statement"] + "\n\nRationale (" +
                       preview["rationale"]["author_role"] + "):\n" + preview["rationale"]["summary"] +
                       "\n\nIntent-ID: " + preview["decision_id"] + "\n")
            validate_intent_commit(repository, message, preview["tree"], preview["base"])
            arguments = ["commit-tree", preview["tree"]]
            if preview["base"]:
                arguments += ["-p", preview["base"]]
            revision = repository.git(*arguments, data=message.encode()).strip()
            repository.git("update-ref", "-m", "canvas: checkpoint reviewed intent", "HEAD", revision,
                           preview["base"] or "0" * 40)
            try:
                os.replace(next_index, index_path)
            except OSError:
                repository.git("update-ref", "HEAD", preview["base"] or "0" * 40, revision)
                raise
        self.previews.pop(preview["token"], None)
        return self.inspect_commit(revision)


def validate_intent_commit(repository, message, tree=None, base=None):
    import re
    ids = re.findall(r"^Intent-ID:\s*(\S+)\s*$", message, re.MULTILINE)
    if not ids:
        raise GitError("Add an Intent-ID trailer linking this commit to a recorded human decision.")
    target = tree or ":"
    document = json.loads(repository.git("show", target + (":.canvas/decisions.json" if tree else ".canvas/decisions.json")))
    records = {record["id"]: record for record in document["decisions"]}
    previous = base or repository.head()
    if previous:
        prior = json.loads(repository.git("show", previous + ":.canvas/decisions.json", check=False) or '{"decisions":[]}')
        for record in prior["decisions"]:
            successor = records.get(record["id"])
            if not successor or successor["statement"] != record["statement"] or successor.get("origin") != record.get("origin"):
                raise GitError("Human statements and their origins are immutable; add a clarifying decision instead.")
    if any(record.get("author", {}).get("role") != "human" for record in records.values()):
        raise GitError("Every decision must identify its human source.")
    if any(identity not in records for identity in ids):
        raise GitError("Every Intent-ID must exist in the decision document committed with this change.")
    if tree:
        empty = repository.git("hash-object", "-t", "tree", "-w", "--stdin", data=b"").strip()
        paths = repository.git("diff", "--name-only", "-z", base or empty, tree).split("\0")
    else:
        paths = repository.git("diff", "--cached", "--name-only", "-z").split("\0")
    for path in paths:
        if not path or path.startswith(".canvas/"):
            continue
        bindings = [records[identity] for identity in ids if any(fnmatch.fnmatchcase(path, pattern)
                     for pattern in records[identity].get("artifacts", []))]
        if not bindings:
            raise GitError("No selected human decision covers artifact: " + path)
        if not any(record.get("rationale", {}).get("summary", "").strip() for record in bindings):
            raise GitError("Record a rationale for the artifact change: " + path)
    return True
