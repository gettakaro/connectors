"""The TLS fake Takaro a ``wss://``-only connector is verified against."""

from __future__ import annotations

import asyncio
import json
import ssl
from pathlib import Path

import pytest
from websockets.asyncio.client import connect

from takaro_maint.verify.fake_takaro import FakeTakaro
from takaro_maint.verify.tls import make_test_pki


def _client_context(ca: Path) -> ssl.SSLContext:
    context = ssl.create_default_context(cafile=str(ca))
    context.check_hostname = True
    return context


def test_a_tls_fake_is_trusted_through_its_own_ca_only(tmp_path: Path) -> None:
    pki = make_test_pki(tmp_path / "pki", "host.docker.internal")
    assert oct((tmp_path / "pki").stat().st_mode)[-3:] == "700", "the CA key directory is private"
    assert "PRIVATE KEY" not in pki.ca_pem.read_text(), "the file handed to the connector is public"

    async def scenario() -> None:
        fake = FakeTakaro(host="127.0.0.1", ssl_context=pki.context)
        port = await fake.start()
        assert fake.url.startswith("wss://")
        try:
            uri = f"wss://127.0.0.1:{port}/"
            async with connect(uri, ssl=_client_context(pki.ca_pem), server_hostname=pki.hostname) as ws:
                await ws.send(json.dumps({"type": "identify", "payload": {"identityToken": "t"}}))
                assert json.loads(await ws.recv())["type"] == "identifyResponse"
                # The application heartbeat is answered the way real Takaro answers it.
                await ws.send(json.dumps({"type": "ping"}))
                pong = json.loads(await ws.recv())
                assert pong["type"] == "pong" and pong["payload"] is None and pong["requestId"]
                assert fake.app_pings == 1
                # The empty RFC 6455 ping real Takaro sends.
                assert await fake.ping(timeout=5, payload=b"") >= 0

            # Anyone who does not trust this run's CA is refused.
            with pytest.raises(ssl.SSLCertVerificationError):
                async with connect(uri, ssl=ssl.create_default_context(), server_hostname=pki.hostname):
                    pass
            # And the certificate names only the host the container reaches the fake by.
            with pytest.raises(ssl.SSLCertVerificationError):
                async with connect(uri, ssl=_client_context(pki.ca_pem), server_hostname="elsewhere.invalid"):
                    pass
        finally:
            await fake.stop()

    asyncio.run(scenario())


def test_a_plain_fake_stays_plain() -> None:
    assert FakeTakaro(host="127.0.0.1", port=1).url == "ws://127.0.0.1:1/"


def test_a_protocol_1_fake_negotiates_acks_and_raises_a_response_error() -> None:
    """What a protocol 1 connector needs from the fake: the version, eventAck, the manifest answer."""

    async def scenario() -> None:
        fake = FakeTakaro(host="127.0.0.1", protocol=1)
        port = await fake.start()
        try:
            async with connect(f"ws://127.0.0.1:{port}/") as ws:
                await ws.send(
                    json.dumps({"type": "identify", "payload": {"identityToken": "t", "protocolVersions": [0, 1]}})
                )
                answer = json.loads(await ws.recv())
                assert answer["payload"]["protocolVersion"] == 1 and fake.negotiated_protocol == 1

                await ws.send(json.dumps({"type": "gameEvent", "payload": {"type": "log", "streamId": "s", "seq": 3}}))
                assert json.loads(await ws.recv()) == {"type": "eventAck", "payload": {"streamId": "s", "upTo": 3}}

                await ws.send(json.dumps({"type": "capabilities", "requestId": "r1", "payload": {"capabilities": []}}))
                assert json.loads(await ws.recv()) == {
                    "type": "capabilitiesResponse",
                    "payload": None,
                    "requestId": "r1",
                }
                assert fake.capability_updates == [{"capabilities": []}]

                pending = asyncio.ensure_future(fake.request("getPlayers"))
                request = json.loads(await ws.recv())
                error = {"code": "game_error", "message": "not loaded"}
                await ws.send(json.dumps({"type": "response", "requestId": request["requestId"], "error": error}))
                with pytest.raises(RuntimeError, match="not loaded"):
                    await pending

            # A connector that offers no protocol list is answered the protocol 0 way.
            async with connect(f"ws://127.0.0.1:{port}/") as ws:
                await ws.send(json.dumps({"type": "identify", "payload": {"identityToken": "t"}}))
                assert "protocolVersion" not in json.loads(await ws.recv())["payload"]
                assert fake.negotiated_protocol == 0
        finally:
            await fake.stop()

    asyncio.run(scenario())
