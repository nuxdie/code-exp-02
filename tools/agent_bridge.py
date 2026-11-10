#!/usr/bin/env python3
"""Connect the native canvas to the user's local Codex app server over JSONL."""

import asyncio
import json
import os
import shutil
from pathlib import Path
import sys


def emit(message):
    print(json.dumps(message, ensure_ascii=False), flush=True)


async def write_stdout(message, fd=1):
    data = memoryview((message + "\n").encode())
    loop = asyncio.get_running_loop()
    while data:
        try:
            count = os.write(fd, data)
            data = data[count:]
        except BlockingIOError:
            ready = loop.create_future()
            loop.add_writer(fd, lambda: ready.done() or ready.set_result(None))
            try:
                await ready
            finally:
                loop.remove_writer(fd)


async def stdin_reader():
    reader = asyncio.StreamReader(limit=64 * 1024 * 1024)
    protocol = asyncio.StreamReaderProtocol(reader)
    await asyncio.get_running_loop().connect_read_pipe(lambda: protocol, sys.stdin.buffer)
    return reader


async def forward_lines(reader, send):
    while line := await reader.readline():
        await send(line.decode().rstrip("\n"))


async def finish_when_either(*coroutines):
    tasks = [asyncio.create_task(coroutine) for coroutine in coroutines]
    done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    for task in pending:
        task.cancel()
    await asyncio.gather(*pending, return_exceptions=True)
    for task in done:
        task.result()


def codex_executable():
    override = os.environ.get("SOURCE_CANVAS_CODEX")
    if override:
        executable = shutil.which(override)
        if executable:
            return executable
        raise FileNotFoundError(f"SOURCE_CANVAS_CODEX executable not found: {override}")
    executable = shutil.which("codex")
    if executable:
        return executable
    # Desktop launchers often omit the per-user installation directory from PATH.
    local = Path.home() / ".local" / "bin" / "codex"
    if local.is_file() and os.access(local, os.X_OK):
        return str(local)
    raise FileNotFoundError(
        "Codex executable not found. Install Codex or set SOURCE_CANVAS_CODEX to its full path.")


async def daemon_socket():
    # Reuse the daemon so the agent keeps running while the canvas reloads or reconnects.
    if path := os.environ.get("SOURCE_CANVAS_AGENT_SOCKET"):
        return path
    process = await asyncio.create_subprocess_exec(
        codex_executable(), "app-server", "daemon", "version",
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
    stdout, _ = await process.communicate()
    if process.returncode == 0:
        state = json.loads(stdout)
        if state.get("status") == "running" and Path(state.get("socketPath", "")).exists():
            return state["socketPath"]
    return None


async def websocket_bridge(path, reader):
    import websockets
    async with websockets.unix_connect(path, uri="ws://localhost", max_size=64 * 1024 * 1024) as socket:
        emit({"method": "canvas/connected", "params": {}})

        async def receive():
            async for message in socket:
                await write_stdout(message)

        await finish_when_either(forward_lines(reader, socket.send), receive())


async def stdio_bridge(reader):
    process = await asyncio.create_subprocess_exec(
        codex_executable(), "app-server", "--listen", "stdio://", stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=sys.stderr, limit=64 * 1024 * 1024)
    emit({"method": "canvas/connected", "params": {}})

    async def send(message):
        process.stdin.write((message + "\n").encode())
        await process.stdin.drain()

    async def receive():
        while line := await process.stdout.readline():
            await write_stdout(line.decode().rstrip("\n"))

    try:
        await finish_when_either(forward_lines(reader, send), receive())
    finally:
        if process.returncode is None:
            process.terminate()
            await process.wait()


async def main():
    os.set_blocking(sys.stdout.fileno(), False)
    reader = await stdin_reader()
    path = await daemon_socket()
    if path:
        await websocket_bridge(path, reader)
    else:
        await stdio_bridge(reader)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    except Exception as error:
        emit({"method": "canvas/disconnected", "params": {"message": str(error)}})
        raise SystemExit(1)
