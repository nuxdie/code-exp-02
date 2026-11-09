"""Verify that the quality gate rejects real policy violations."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("quality", ROOT / "tools" / "quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALITY)


class QualityPolicyTests(unittest.TestCase):
    def test_include_cycle_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first, second = root / "first.h", root / "second.h"
            first.write_text('#include "second.h"\n')
            second.write_text('#include "first.h"\n')
            with patch.object(QUALITY, "ROOT", root):
                with self.assertRaisesRegex(RuntimeError, "Include cycle"):
                    QUALITY.check_architecture([first, second])

    def test_oversized_module_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "oversized.cpp"
            source.write_text("\n" * 751)
            with patch.object(QUALITY, "ROOT", root):
                with self.assertRaisesRegex(RuntimeError, "module budget"):
                    QUALITY.check_architecture([source])

    def test_unformatted_code_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / ".clang-format").write_bytes((ROOT / ".clang-format").read_bytes())
            source = root / "unformatted.cpp"
            source.write_text("int  main(){return 0;}\n")
            result = subprocess.run(
                [QUALITY.tool("clang-format-18", "clang-format"), "--style=file", "--dry-run",
                 "--Werror", str(source)], text=True, capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)

    def test_excessive_complexity_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "complex.cpp"
            conditions = "\n".join(f"if (value > {i}) {{" for i in range(8))
            source.write_text("int complex(int value) {\n" + conditions + "\nreturn value;\n"
                              + "}\n" * 8 + "return 0;\n}\n")
            result = subprocess.run(
                [QUALITY.tool("clang-tidy-18", "clang-tidy"), str(source),
                 "--config-file=" + str(ROOT / ".clang-tidy"), "--", "-std=c++17"],
                text=True, capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cognitive complexity", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
