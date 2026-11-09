#!/usr/bin/env python3
"""Format and lint owned CMake files with one policy for builds and CI."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sources(root=ROOT):
    return sorted(path for path in root.rglob('*') if path.is_file()
                  and (path.name == 'CMakeLists.txt' or path.suffix == '.cmake')
                  and not any(part.startswith('build') or part in {'.git', '.cache', 'assets', 'fonts'}
                              for part in path.relative_to(root).parts))


def check(write=False, root=ROOT):
    formatter = shutil.which('cmake-format')
    linter = shutil.which('cmake-lint')
    if not formatter or not linter:
        raise RuntimeError('Install cmake-format (which includes cmake-lint) to maintain CMake files.')
    files = [str(path) for path in sources(root)]
    if not files:
        return
    config = str(root / '.cmake-format.json')
    subprocess.run([formatter, '--config-files', config, '--in-place' if write else '--check', '--', *files],
                   cwd=root, check=True)
    subprocess.run([linter, '--config-files', config, '--', *files], cwd=root, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    arguments = parser.parse_args()
    try:
        check(arguments.write)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')
