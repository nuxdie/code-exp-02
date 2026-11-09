#!/usr/bin/env python3
"""Run the same mandatory maintainability checks locally and in CI."""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp"}


def sources():
    return sorted(
        path for path in ROOT.rglob("*")
        if path.suffix in CPP_SUFFIXES and path.is_file()
        and not any(part.startswith("build") or part in {".git", ".cache", "assets", "fonts"}
                    for part in path.relative_to(ROOT).parts)
    )


def command(arguments):
    subprocess.run(arguments, cwd=ROOT, check=True)


def tool(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise RuntimeError("Missing required tool: " + " or ".join(names))


def check_architecture(files):
    known = {path.resolve() for path in files}
    graph = {}
    for path in files:
        text = path.read_text()
        if len(text.splitlines()) > 750:
            raise RuntimeError(f"{path.relative_to(ROOT)} exceeds the 750-line module budget")
        includes = re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', text, re.MULTILINE)
        dependencies = []
        for name in includes:
            candidates = [(path.parent / name).resolve(), (ROOT / name).resolve()]
            dependencies.extend(candidate for candidate in candidates if candidate in known)
        graph[path.resolve()] = dependencies
    visited = set()
    active = []

    def visit(path):
        if path in active:
            cycle = active[active.index(path):] + [path]
            raise RuntimeError("Include cycle: " + " -> ".join(str(item.relative_to(ROOT)) for item in cycle))
        if path in visited:
            return
        active.append(path)
        for dependency in graph[path]:
            visit(dependency)
        active.pop()
        visited.add(path)

    for path in graph:
        visit(path)


def analyze_file(path, analyzer, build):
    started = time.monotonic()
    result = subprocess.run(
        [analyzer, str(path), "-p", str(build), "--quiet"], cwd=ROOT,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    report = build / "quality" / (str(path.relative_to(ROOT)).replace("/", "_") + ".log")
    report.write_text(result.stdout)
    return {"file": str(path.relative_to(ROOT)), "passed": result.returncode == 0,
            "seconds": round(time.monotonic() - started, 3), "log": str(report)}


def analyze(build, jobs):
    analyzer = tool("clang-tidy-18", "clang-tidy")
    database = build / "compile_commands.json"
    if not database.exists():
        raise RuntimeError("Compilation database missing; use --build on the first run")
    owned = set(sources())
    files = sorted({Path(entry["file"]).resolve() for entry in json.loads(database.read_text())}
                   & owned)
    missing = {path for path in owned if path.suffix in {".cpp", ".cc", ".cxx", ".c"}} - set(files)
    if missing:
        raise RuntimeError("Sources missing from the build: " + ", ".join(str(path.relative_to(ROOT)) for path in sorted(missing)))
    if not files:
        raise RuntimeError("No project translation units found in the compilation database")
    (build / "quality").mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        reports = list(pool.map(lambda path: analyze_file(path, analyzer, build), files))
    (build / "quality" / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
    failures = [item for item in reports if not item["passed"]]
    for failure in failures:
        print(Path(failure["log"]).read_text(), file=sys.stderr)
    if failures:
        raise RuntimeError(f"Static analysis failed for {len(failures)} translation units")
    print(f"Static analysis passed for {len(reports)} translation units.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--build", action="store_true", help="Configure and compile before analysis")
    parser.add_argument("--jobs", type=int, default=min(3, os.cpu_count() or 1))
    arguments = parser.parse_args()
    build = arguments.build_dir.resolve()
    files = sources()
    command([tool("clang-format-18", "clang-format"), "--style=file", "--dry-run", "--Werror"]
            + [str(path) for path in files])
    check_architecture(files)
    command([sys.executable, str(ROOT / "tools/cmake_style.py")])
    if arguments.build:
        command(["cmake", "-S", str(ROOT), "-B", str(build), "-DBUILD_TESTING=ON"])
        command(["cmake", "--build", str(build), "--parallel", str(max(1, arguments.jobs))])
    analyze(build, max(1, arguments.jobs))
    command(["ctest", "--test-dir", str(build), "--output-on-failure"])
    print("Quality gate passed: format, module size, include cycles, static analysis and tests.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Quality gate failed: {error}", file=sys.stderr)
        sys.exit(1)
