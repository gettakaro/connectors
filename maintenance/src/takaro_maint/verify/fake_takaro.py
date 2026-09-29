"""A local stand-in for Takaro that speaks the Generic Connector protocol.

The frame shapes are the ones ``games/7d2d/tests/fixtures/generic-protocol.json`` pins:
an ``identify`` handshake, ``request``/``response`` envelopes carrying a ``requestId`` and
a JSON-string ``args``, and ``gameEvent`` pushes from the connector.
"""

from __future__ import annotations

import asyncio
import json
import ssl
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import websockets
from websockets.asyncio.server import Server, ServerConnection, serve

from .. import redact


@dataclass
class FakeTakaro:
    """One connector connection at a time, which is all a single server needs."""

    host: str = "127.0.0.1"
    port: int = 0
    log_path: Path | None = None
    game_server_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    #: Serve ``wss://`` with this context; ``None`` is plain ``ws://``.
    ssl_context: ssl.SSLContext | None = None

    _server: Server | None = field(default=None, init=False)
    _connection: ServerConnection | None = field(default=None, init=False)
    _pending: dict[str, asyncio.Future[Any]] = field(default_factory=dict, init=False)
    identified: dict[str, Any] | None = field(default=None, init=False)
    identify_count: int = field(default=0, init=False)
    app_pings: int = field(default=0, init=False)
    events: list[dict[str, Any]] = field(default_factory=list, init=False)

    async def start(self) -> int:
        self._server = await serve(self._handle, self.host, self.port, ssl=self.ssl_context)
        self.port = next(iter(self._server.sockets)).getsockname()[1]
        return self.port

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    @property
    def url(self) -> str:
        scheme = "wss" if self.ssl_context is not None else "ws"
        return f"{scheme}://{self.host}:{self.port}/"

    def _log(self, direction: str, frame: Any) -> None:
        if self.log_path is None:
            return
        line = json.dumps({"direction": direction, "frame": frame}, ensure_ascii=False)
        tokens = []
        if isinstance(frame, dict) and isinstance(frame.get("payload"), dict):
            tokens = [
                str(value)
                for key, value in frame["payload"].items()
                if "token" in key.lower() and isinstance(value, str)
            ]
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with self.log_path.open("a", encoding="utf-8") as handle:
            handle.write(redact.redact(line, tokens) + "\n")

    async def _send(self, connection: ServerConnection, frame: dict[str, Any]) -> None:
        self._log("out", frame)
        await connection.send(json.dumps(frame))

    async def _handle(self, connection: ServerConnection) -> None:
        self._connection = connection
        try:
            async for raw in connection:
                try:
                    frame = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                self._log("in", frame)
                await self._dispatch(connection, frame)
        except websockets.ConnectionClosed:
            pass
        finally:
            if self._connection is connection:
                self._connection = None

    async def _dispatch(self, connection: ServerConnection, frame: dict[str, Any]) -> None:
        kind = frame.get("type")
        if kind == "identify":
            self.identified = frame.get("payload", {})
            self.identify_count += 1
            await self._send(
                connection,
                {
                    "type": "identifyResponse",
                    "payload": {"gameServerId": self.game_server_id, "status": "authenticated"},
                },
            )
            return
        if kind in ("response", "error"):
            future = self._pending.pop(str(frame.get("requestId")), None)
            if future is not None and not future.done():
                if kind == "error":
                    future.set_exception(RuntimeError(json.dumps(frame.get("payload"))))
                else:
                    future.set_result(frame.get("payload"))
            return
        if kind == "gameEvent":
            self.events.append(frame.get("payload", {}))
            return
        # The application heartbeat some connectors send; real Takaro answers it like this,
        # and a connector that confirms delivery by a later pong depends on the answer.
        if kind == "ping":
            self.app_pings += 1
            await self._send(connection, {"type": "pong", "payload": None, "requestId": str(uuid.uuid4())})

    async def wait_for_identify(self, timeout: float, *, minimum: int = 1) -> dict[str, Any]:
        """Resolve once at least ``minimum`` identify frames have arrived (the first by default).

        Counting rather than waiting on a one-shot event is what lets a caller wait for the
        identify that follows a reconnect or a second boot.
        """
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while self.identify_count < minimum:
            if loop.time() >= deadline:
                raise TimeoutError(f"only {self.identify_count} identify frame(s) arrived, expected {minimum}")
            await asyncio.sleep(0.1)
        assert self.identified is not None
        return self.identified

    async def disconnect(self, code: int = 1001, reason: str = "going away") -> None:
        """Close the connector's socket from the Takaro side; the connector is expected to come back."""
        connection = self._connection
        if connection is not None:
            await connection.close(code=code, reason=reason)

    async def request(self, action: str, args: dict[str, Any] | None = None, *, timeout: float = 30.0) -> Any:
        """Send one ``request`` frame and resolve on its matching ``response``."""
        if self._connection is None:
            raise RuntimeError("no connector is connected")
        request_id = str(uuid.uuid4())
        future: asyncio.Future[Any] = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        await self._send(
            self._connection,
            {
                "type": "request",
                "requestId": request_id,
                "payload": {"action": action, "args": json.dumps(args or {})},
            },
        )
        try:
            return await asyncio.wait_for(future, timeout)
        finally:
            self._pending.pop(request_id, None)

    async def ping(self, *, timeout: float = 5.0, payload: bytes | None = None) -> float:
        """One RFC 6455 ping, awaited to its pong. Returns the round trip in seconds.

        ``payload=b""`` sends the empty ping real Takaro sends; the default is a random one.
        """
        if self._connection is None:
            raise RuntimeError("no connector is connected")
        loop = asyncio.get_running_loop()
        started = loop.time()
        pong_waiter = await self._connection.ping(payload)
        await asyncio.wait_for(pong_waiter, timeout)
        return loop.time() - started
