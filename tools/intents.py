#!/usr/bin/env python3
"""Intent store: who asked for each line, in their words, and why it was written that way.

The store is language-agnostic. It knows three things:

  intent  a short durable decision, close to the human's own wording
  turn    one thing the human said, what the agent answered, and the lines that turn added
  blame   for a line in the working tree, the turn (and so the intent) that introduced it

Lines are identified by a hash of their text with whitespace removed, so attribution
survives reformatting, moves between files, and edits around them.
"""

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

STORE = ".canvas/intents.json"
MILESTONE = 3  # a commit serving more statements than this is weak evidence for any one
DISTINCTIVE = 24  # normalized characters before a line may be matched across files
CONTEXT = re.compile(r"<external_source-canvas>(.*?)</external_source-canvas>", re.S)


def normalize(line):
    return "".join(line.split())


def line_hash(line):
    text = normalize(line)
    if len(text) < 4:
        return None
    return hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:7]


def added_lines(change):
    if change.get("type") == "add":
        return change.get("content", "").splitlines()
    return [line[1:] for line in change.get("unified_diff", "").splitlines()
            if line.startswith("+") and not line.startswith("+++")]


def hashes(lines):
    seen, result = set(), []
    for line in lines:
        value = line_hash(line)
        if value and value not in seen:
            seen.add(value)
            result.append(value)
    return result


def human_text(content):
    """Split a recorded user message into what the human typed and where they were."""
    said, where = [], None
    for part in content:
        text = part.get("text", "") if isinstance(part, dict) else ""
        found = CONTEXT.search(text)
        if found:
            try:
                context = json.loads(found.group(1))
                if context.get("path"):
                    where = {"path": context["path"], "line": context.get("line", 1)}
            except ValueError:
                pass
            text = CONTEXT.sub("", text)
        if text.startswith(("<environment_context", "# AGENTS.md", "<turn_aborted",
                            "<user_instructions")):
            continue
        if text.strip():
            said.append(text.strip())
    return "\n".join(said), where


def question_reply(text):
    found = re.search(r"<send_user_message_question_reply>\s*(.*?)\s*</send_user", text, re.S)
    if not found:
        return text
    try:
        return "; ".join(f"{item['question']} → {item['answer']}"
                         for item in json.loads(found.group(1)))
    except (ValueError, KeyError):
        return text


def codex_turns(root, sessions):
    """Read every Codex transcript recorded in this project and return its turns."""
    root = str(Path(root).resolve())
    turns = {}
    names = [name for folder in sessions
             for name in glob.glob(os.path.join(folder, "**", "*.jsonl"), recursive=True)]
    for name in sorted(names, key=os.path.basename):
        with open(name, encoding="utf-8", errors="replace") as stream:
            try:
                meta = json.loads(stream.readline()).get("payload", {})
            except ValueError:
                continue
            if meta.get("cwd") != root:
                continue
            for raw in stream:
                try:
                    record = json.loads(raw)
                except ValueError:
                    continue
                if record.get("type") != "event_msg":
                    continue
                payload = record.get("payload", {})
                turn_id = payload.get("turn_id")
                if not turn_id:
                    continue
                turn = turns.setdefault(turn_id, {
                    "id": turn_id, "thread": meta.get("id"), "at": record.get("timestamp"),
                    "source": "transcript", "said": "", "why": "", "changes": {}})
                if payload.get("type") == "task_complete":
                    turn["why"] = payload.get("last_agent_message") or turn["why"]
                    continue
                if payload.get("type") != "item_completed":
                    continue
                item = payload.get("item", {})
                if item.get("type") == "UserMessage":
                    said, where = human_text(item.get("content", []))
                    if said:
                        turn["said"] = (turn["said"] + "\n" + question_reply(said)).strip()
                    if where:
                        turn["where"] = where
                elif item.get("type") == "AgentMessage" and item.get("phase") == "final_answer":
                    turn["why"] = "".join(part.get("text", "") for part in item["content"])
                elif item.get("type") == "FileChange":
                    for path, change in item.get("changes", {}).items():
                        for target in {path, change.get("move_path") or path}:
                            if not target.startswith(root + "/"):
                                continue
                            relative = target[len(root) + 1:]
                            turn["changes"].setdefault(relative, []).extend(added_lines(change))
    result = []
    for turn in turns.values():
        if not turn["said"]:
            continue
        turn["changes"] = {path: hashes(lines) for path, lines in turn["changes"].items()}
        turn["changes"] = {path: values for path, values in turn["changes"].items() if values}
        result.append(turn)
    return sorted(result, key=lambda turn: turn["at"] or "")


def git(root, *arguments):
    return subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True,
                          text=True, errors="replace").stdout


def commit_lines(root, commit):
    """Added lines per path for one commit, ignoring the store itself."""
    changes, path = {}, None
    for line in git(root, "show", "--format=", "--unified=0", commit).splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif path and line.startswith("+") and not path.startswith(".canvas/"):
            changes.setdefault(path, []).append(line[1:])
    return {path: hashes(lines) for path, lines in changes.items()}


def add_commits(root, turns, links):
    """Fold in lines that were committed for a turn but never appear in its transcript.

    `links` maps a commit to the turns it served. Committed lines are weaker evidence
    than a transcript's own edits, so they are kept apart and only claim lines no
    transcript explains.
    """
    by_id = {turn["id"]: turn for turn in turns}
    for commit, turn_ids in links.items():
        targets = [by_id[turn_id] for turn_id in turn_ids if turn_id in by_id]
        if not targets:
            continue
        try:
            changes = commit_lines(root, commit)
        except subprocess.CalledProcessError:
            continue
        # A commit that answers many statements at once cannot say which one a line serves.
        strength = "committed" if len(targets) <= MILESTONE else "milestone"
        for turn in targets:
            turn.setdefault("commits", []).append(commit)
            committed = turn.setdefault(strength, {})
            for path, values in changes.items():
                own = set(turn["changes"].get(path, []))
                committed[path] = list(dict.fromkeys(
                    committed.get(path, []) + [value for value in values if value not in own]))


EVIDENCE = ("changes", "committed", "milestone")
FIELDS = ("id", "thread", "at", "source", "said", "why", "where", "intent", "also", "commits",
          *EVIDENCE)


def load(root):
    path = Path(root) / STORE
    if not path.exists():
        return {"version": 2, "intents": [], "commits": {}, "turns": []}
    store = json.loads(path.read_text())
    for turn in store["turns"]:
        for evidence in EVIDENCE:
            if evidence in turn:
                turn[evidence] = {name: text.split() for name, text in turn[evidence].items()}
    return store


def save(root, store):
    path = Path(root) / STORE
    turns = []
    for turn in store["turns"]:
        turn = {key: turn[key] for key in FIELDS if turn.get(key) not in (None, [], {})
                or key in ("intent", "changes")}
        for evidence in EVIDENCE:
            if evidence in turn:
                turn[evidence] = {name: " ".join(values)
                                  for name, values in sorted(turn[evidence].items()) if values}
        turns.append(turn)
    text = json.dumps({**store, "turns": turns}, ensure_ascii=False, indent=1) + "\n"
    # The canvas watches the project; rewriting an unchanged store would wake it for nothing.
    if not path.exists() or path.read_text() != text:
        if turns or store["intents"]:
            path.parent.mkdir(exist_ok=True)
            path.write_text(text)


def rebuild(root, sessions):
    """Rebuild turns from the evidence on disk, keeping distilled intents and their links."""
    store = load(root)
    previous = {turn["id"]: (turn.get("intent"), turn.get("also")) for turn in store["turns"]}
    turns = codex_turns(root, sessions)
    found = {turn["id"] for turn in turns}
    # Transcripts live on the machine that ran them; keep turns imported from elsewhere.
    for turn in store["turns"]:
        if turn["id"] not in found:
            turns.append({key: value for key, value in turn.items()
                          if key not in ("committed", "milestone", "commits")})
    add_commits(root, turns, store.get("commits", {}))
    turns.sort(key=lambda turn: turn["at"] or "")
    for turn in turns:
        turn["intent"], also = previous.get(turn["id"], (None, None))
        turn.pop("also", None)
        if also:
            turn["also"] = also
    store["turns"] = turns
    save(root, store)
    return store


def working_changes(root, base):
    """Lines added since `base`, including files Git does not track yet."""
    changes, path = {}, None
    for line in git(root, "diff", "--unified=0", base).splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif path and line.startswith("+") and path != STORE:
            changes.setdefault(path, []).append(line[1:])
    for path in git(root, "ls-files", "--others", "--exclude-standard").splitlines():
        if path == STORE:
            continue
        try:
            changes[path] = (Path(root) / path).read_text().splitlines()
        except (UnicodeDecodeError, OSError):
            continue
    return {path: hashes(lines) for path, lines in changes.items() if hashes(lines)}


def record(root, said, why, intent, base, identifier):
    """Record a turn from the working tree, for agents whose transcripts are not read here.

    Recording the same id again replaces that turn, so an agent can call this after
    every step of a long answer.
    """
    store = load(root)
    if intent and intent not in {item["id"] for item in store["intents"]}:
        raise SystemExit("Unknown intent: " + intent)
    stamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    identifier = identifier or "turn-" + hashlib.sha1((stamp + said).encode()).hexdigest()[:12]
    turns = [turn for turn in store["turns"] if turn["id"] != identifier]
    previous = next((turn for turn in store["turns"] if turn["id"] == identifier), {})
    turns.append({"id": identifier, "thread": previous.get("thread"),
                  "at": previous.get("at", stamp), "source": "transcript", "said": said,
                  "why": why, "changes": working_changes(root, base), "intent": intent or None})
    store["turns"] = sorted(turns, key=lambda turn: turn["at"] or "")
    save(root, store)
    return identifier


def tracked_files(root):
    """The project's files: what Git tracks, or everything visible when there is no Git."""
    try:
        return [name for name in git(root, "ls-files").splitlines() if name]
    except (subprocess.CalledProcessError, OSError):
        names = []
        for folder, folders, files in os.walk(root):
            folders[:] = [name for name in folders if not name.startswith(".")]
            names += [os.path.relpath(os.path.join(folder, name), root) for name in files
                      if not name.startswith(".")]
        return sorted(names)


def blame_file(lines, path, same_path, anywhere, weak):
    """Attribute each line to the turn that first introduced its text."""
    owners, unsure = [], []
    for line in lines:
        value = line_hash(line)
        owner = None
        if value:
            owner = same_path.get((path, value))
            if owner is None and len(normalize(line)) >= DISTINCTIVE:
                owner = anywhere.get(value)
        owners.append(owner)
        unsure.append(owner is None and value is not None and (path, value) in weak)
    # Lines known only from a many-statement commit belong with the code around them.
    for order in (range(len(lines)), range(len(lines) - 1, -1, -1)):
        last = None
        for index in order:
            if owners[index] is not None:
                last = owners[index]
            elif unsure[index] and last is not None:
                owners[index] = last
    for index, line in enumerate(lines):
        if unsure[index] and owners[index] is None:
            owners[index] = weak[(path, line_hash(line))]
    # So do lines too short to identify: braces and the like.
    last = None
    for index, line in enumerate(lines):
        if owners[index] is not None:
            last = owners[index]
        elif last is not None and line.strip() and not line_hash(line):
            owners[index] = last
    # A blank line inside one turn's code is part of it; between two turns it is a boundary.
    index = 0
    while index < len(lines):
        end = index
        while end < len(lines) and owners[end] is None and not lines[end].strip():
            end += 1
        if end > index and 0 < index and end < len(lines) and owners[index - 1] == owners[end]:
            owners[index:end] = [owners[end]] * (end - index)
        index = max(end, index + 1)
    return owners


def blame(root, store=None):
    """Return {path: [turn index or None per line]} for every tracked text file."""
    store = store or load(root)
    same_path, anywhere, weak = {}, {}, {}
    for evidence in ("changes", "committed"):  # a transcript's own edits win over commits
        for index, turn in enumerate(store["turns"]):
            for path, values in turn.get(evidence, {}).items():
                for value in values:
                    same_path.setdefault((path, value), index)
        for index, turn in enumerate(store["turns"]):
            for values in turn.get(evidence, {}).values():
                for value in values:
                    anywhere.setdefault(value, index)
    for index, turn in enumerate(store["turns"]):
        for path, values in turn.get("milestone", {}).items():
            for value in values:
                weak.setdefault((path, value), index)
    result = {}
    for path in tracked_files(root):
        try:
            lines = (Path(root) / path).read_text().splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        result[path] = blame_file(lines, path, same_path, anywhere, weak)
    return result


def ranges(owners):
    """Collapse per-line owners into [first_line, last_line, turn] runs (1-based)."""
    result, start = [], 0
    for index in range(1, len(owners) + 1):
        if index == len(owners) or owners[index] != owners[start]:
            if owners[start] is not None:
                result.append([start + 1, index, owners[start]])
            start = index
    return result


def view(root):
    """Everything the canvas needs: intents, turns without hashes, and line ranges per file."""
    store = load(root)
    attribution = blame(root, store)
    turns = [{key: value for key, value in turn.items() if key not in EVIDENCE}
             for turn in store["turns"]]
    return {"intents": store["intents"], "turns": turns,
            "files": {path: ranges(owners) for path, owners in attribution.items()
                      if any(owner is not None for owner in owners)}}


def coverage(root):
    store = load(root)
    total = attributed = with_intent = 0
    for owners in blame(root, store).values():
        total += len(owners)
        attributed += sum(owner is not None for owner in owners)
        with_intent += sum(owner is not None and bool(store["turns"][owner].get("intent"))
                           for owner in owners)
    return {"lines": total, "attributed": attributed, "with_intent": with_intent,
            "turns": len(store["turns"]), "intents": len(store["intents"]),
            "turns_without_intent": sum(not turn.get("intent") for turn in store["turns"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["rebuild", "view", "coverage", "turns", "record"])
    parser.add_argument("--refresh", action="store_true",
                        help="view: re-read transcripts first, so turns in progress appear")
    parser.add_argument("--said", help="record: the human's words, verbatim")
    parser.add_argument("--why", default="", help="record: the agent's reasoning for this turn")
    parser.add_argument("--intent", help="record: id of the intent this turn serves")
    parser.add_argument("--base", default="HEAD", help="record: revision the turn started from")
    parser.add_argument("--id", help="record: stable turn id; repeating it replaces the turn")
    parser.add_argument("--root", default=".")
    parser.add_argument("--sessions", action="append", default=[],
                        help="transcript folder; repeat to add folders copied from other machines")
    options = parser.parse_args()
    if options.command == "rebuild":
        rebuild(options.root, options.sessions or [os.path.expanduser("~/.codex/sessions")])
        print(json.dumps(coverage(options.root)))
    elif options.command == "record":
        if not options.said:
            parser.error("record needs --said")
        print(record(options.root, options.said, options.why, options.intent, options.base,
                     options.id))
    elif options.command == "view":
        if options.refresh:
            rebuild(options.root, options.sessions or [os.path.expanduser("~/.codex/sessions")])
        print(json.dumps(view(options.root), ensure_ascii=False))
    elif options.command == "coverage":
        print(json.dumps(coverage(options.root)))
    else:
        for turn in load(options.root)["turns"]:
            print(f"{turn['id'][:13]} {turn['source'][:1]} {len(turn['changes']):3} "
                  f"{len(turn.get('committed', {})):3} {len(turn.get('milestone', {})):3} "
                  f"{turn.get('intent') or '-':28} {turn['said'][:110]!r}")


if __name__ == "__main__":
    main()
