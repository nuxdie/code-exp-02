"""CMake style enforcement must repair formatting and reject remaining warnings."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import cmake_style


class CMakeStyleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / '.cmake-format.json').write_text('{"format":{"line_width":80,"tab_size":4}}')

    def test_build_repairs_layout_and_check_detects_drift(self):
        source = self.root / 'CMakeLists.txt'
        source.write_text('set(  ANSWER   42 )\n')
        with self.assertRaises(subprocess.CalledProcessError):
            cmake_style.check(root=self.root)
        cmake_style.check(write=True, root=self.root)
        self.assertEqual(source.read_text(), 'set(ANSWER 42)\n')
        cmake_style.check(root=self.root)

    def test_lint_warnings_still_fail_after_formatting(self):
        (self.root / 'CMakeLists.txt').write_text('foreach(INVALID_NAME a b)\nmessage("${INVALID_NAME}")\nendforeach()\n')
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            cmake_style.check(write=True, root=self.root)
        self.assertTrue(raised.exception.cmd[0].endswith('cmake-lint'))

    def test_generated_and_cached_files_are_excluded(self):
        for name in ['CMakeLists.txt', 'cmake/Helper.cmake', 'build/generated.cmake',
                     '.cache/dependency/CMakeLists.txt']:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('set(ANSWER 42)\n')
        self.assertEqual([p.relative_to(self.root).as_posix() for p in cmake_style.sources(self.root)],
                         ['CMakeLists.txt', 'cmake/Helper.cmake'])


if __name__ == '__main__':
    unittest.main()
