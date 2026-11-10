import asyncio
import importlib.util
import os
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("agent_bridge", ROOT / "tools/agent_bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    def test_desktop_path_finds_user_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ".local/bin/codex"
            executable.parent.mkdir(parents=True)
            executable.write_text("#!/bin/sh\nexit 0\n")
            executable.chmod(0o755)
            with patch.dict(os.environ, {"PATH": directory}, clear=True), \
                    patch.object(bridge.Path, "home", return_value=Path(directory)):
                self.assertEqual(bridge.codex_executable(), str(executable))

    def test_explicit_executable_takes_priority(self):
        with patch.dict(os.environ, {"SOURCE_CANVAS_CODEX": "/custom/codex"}), \
                patch.object(bridge.shutil, "which", return_value="/custom/codex") as which:
            self.assertEqual(bridge.codex_executable(), "/custom/codex")
            which.assert_called_once_with("/custom/codex")

    def test_missing_executable_explains_configuration(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(bridge.shutil, "which", return_value=None), \
                patch.object(bridge.Path, "is_file", return_value=False):
            with self.assertRaisesRegex(FileNotFoundError, "SOURCE_CANVAS_CODEX"):
                bridge.codex_executable()

    async def test_large_unicode_message_survives_backpressure(self):
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK)
        message = '{"history": "' + "conversation 🌍 " * 160000 + '"}'
        expected = (message + "\n").encode()
        received = bytearray()
        loop = asyncio.get_running_loop()
        complete = loop.create_future()

        def consume():
            try:
                while chunk := os.read(read_fd, 65536):
                    received.extend(chunk)
            except BlockingIOError:
                pass
            if len(received) == len(expected) and not complete.done():
                complete.set_result(None)

        loop.add_reader(read_fd, consume)
        try:
            await asyncio.wait_for(bridge.write_stdout(message, write_fd), 5)
            await asyncio.wait_for(complete, 5)
            self.assertEqual(received, expected)
        finally:
            loop.remove_reader(read_fd)
            os.close(read_fd)
            os.close(write_fd)


if __name__ == "__main__":
    unittest.main()
