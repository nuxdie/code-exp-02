"""Read immutable Git evidence without invoking external diff or text-conversion drivers."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


class GitError(RuntimeError):
    pass


class Repository:
    def __init__(self, root):
        self.root = Path(root).resolve()

    def git(self, *arguments, data=None, environment=None, check=True):
        result = subprocess.run(
            ["git", "-C", str(self.root), "--no-pager", "-c", "color.ui=false",
             "-c", "core.quotepath=false", *arguments], input=data,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
            env=environment)
        if check and result.returncode:
            raise GitError(result.stderr.decode("utf-8", "replace").strip())
        return result.stdout.decode("utf-8", "replace")

    def path(self, path):
        candidate = Path(path)
        if candidate.is_absolute():
            candidate = candidate.relative_to(self.root)
        if not candidate.parts or any(part in {"..", ".git", ".cache"} for part in candidate.parts):
            raise GitError("The artifact must be a project file outside Git and cache metadata.")
        return candidate.as_posix()

    def head(self):
        value = self.git("rev-parse", "--verify", "HEAD", check=False).strip()
        return value if re.fullmatch(r"[0-9a-f]{40,64}", value) else None

    def require_repository(self):
        top = self.git("rev-parse", "--show-toplevel").strip()
        if Path(top).resolve() != self.root:
            raise GitError("Open the repository root to inspect its decision history.")

    def commit(self, revision):
        if not re.fullmatch(r"[0-9a-f]{7,64}", revision):
            raise GitError("Invalid commit identifier.")
        raw = self.git("show", "-s", "--format=%H%x00%s%x00%B", revision)
        commit, subject, message = raw.split("\0", 2)
        return {"commit": commit, "subject": subject, "message": message,
                "intent_ids": re.findall(r"^Intent-ID:\s*(\S+)\s*$", message, re.MULTILINE),
                "reconstructed": "History-Reconstruction:" in message}

    def commits(self):
        if not self.head():
            return []
        raw = self.git("log", "--format=%H%x00%s%x00%B%x00", "--max-count=1000")
        parts = raw.split("\0")
        commits = []
        for index in range(0, len(parts) - 2, 3):
            commit, subject, message = parts[index:index + 3]
            commits.append({"commit": commit.strip(), "subject": subject,
                            "intent_ids": re.findall(r"^Intent-ID:\s*(\S+)\s*$", message, re.MULTILINE),
                "reconstructed": "History-Reconstruction:" in message})
        return commits

    def changes(self, revision):
        raw = self.git("diff-tree", "--root", "--no-commit-id", "--no-renames",
                       "--name-status", "-r", "-z", revision).split("\0")
        return [{"status": raw[i], "path": raw[i + 1]} for i in range(0, len(raw) - 1, 2)
                if not raw[i + 1].startswith(".canvas/")]

    def diff(self, revision, path=None):
        arguments = ["show", "--format=", "--no-ext-diff", "--no-textconv", "--unified=3", revision, "--"]
        if path:
            arguments.append(self.path(path))
        else:
            arguments += [".", ":(exclude).canvas/**"]
        return self.git(*arguments)

    def working_paths(self):
        paths = set(self.git("diff", "--name-only", "-z", "HEAD", "--", ".", check=False).split("\0"))
        if not self.head():
            paths.update(self.git("ls-files", "-z").split("\0"))
        paths.update(self.git("ls-files", "--others", "--exclude-standard", "-z").split("\0"))
        return sorted(path for path in paths if path and not path.startswith(".canvas/"))

    def snapshot(self, paths):
        cache = self.root / ".cache"
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="intent-preview-", dir=cache) as directory:
            index = Path(directory) / "index"
            environment = dict(os.environ, GIT_INDEX_FILE=str(index))
            base = self.head()
            if base:
                self.git("read-tree", base, environment=environment)
            else:
                self.git("read-tree", "--empty", environment=environment)
            self.git("add", "-A", "--", *paths, environment=environment)
            tree = self.git("write-tree", environment=environment).strip()
        empty = self.git("hash-object", "-t", "tree", "-w", "--stdin", data=b"").strip()
        artifacts = [p for p in paths if not p.startswith(".canvas/")]
        patch = self.git("diff", "--no-ext-diff", "--no-textconv", "--unified=3", base or empty,
                         tree, "--", *(artifacts or [".", ":(exclude).canvas/**"]))
        return {"base": base, "tree": tree, "patch": patch,
                "digest": hashlib.sha256(patch.encode()).hexdigest()}

    def line(self, path, line):
        path = self.path(path)
        if line < 1:
            raise GitError("Source lines are numbered from one.")
        source = (self.root / path).read_text(errors="replace").splitlines()
        if line > len(source):
            raise GitError("The selected line is no longer in the current file.")
        tracked = self.git("ls-files", "--error-unmatch", "--", path, check=False).strip()
        in_head = self.git("ls-tree", "HEAD", "--", path, check=False).strip() if self.head() else ""
        if not tracked or not in_head:
            return {"path": path, "line": line, "status": "uncommitted" if tracked else "untracked", "text": source[line - 1],
                    "commit": None, "patch": "", "evolution": ""}
        raw = self.git("blame", "--line-porcelain", "-M", "-C", "-C", "-L", f"{line},{line}", "--", path)
        lines = raw.splitlines()
        header = lines[0].split()
        revision = header[0].lstrip("^")
        metadata = {}
        for item in lines[1:]:
            if item.startswith("\t"):
                break
            key, _, value = item.partition(" ")
            metadata[key] = value
        dirty = not revision.strip("0")
        origin_path = metadata.get("filename", path)
        if origin_path.startswith('"'):
            origin_path = json.loads(origin_path)
        patch = self.git("diff", "--no-ext-diff", "--no-textconv", "--unified=3", "HEAD", "--", path) if dirty else self.diff(revision, origin_path)
        evolution = ""
        if not dirty:
            # Blame finds copied/renamed origins; Git's -L follows the selected line backwards.
            evolution = self.git("log", "--no-ext-diff", "--no-textconv", "--max-count=12",
                                 "--format=commit %H%n%s", f"-L{header[1]},{header[1]}:{origin_path}", revision, check=False)
        return {"path": path, "origin_path": origin_path, "line": line, "original_line": int(header[1]),
                "status": "uncommitted" if dirty else "committed", "commit": None if dirty else revision,
                "text": source[line - 1], "author": metadata.get("author", ""),
                "summary": metadata.get("summary", ""), "patch": patch, "evolution": evolution}
