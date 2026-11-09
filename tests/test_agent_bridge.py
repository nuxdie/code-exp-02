import asyncio
import importlib.util
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("agent_bridge", ROOT / "tools/agent_bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeTests(unittest.IsolatedAsyncioTestCase):
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
