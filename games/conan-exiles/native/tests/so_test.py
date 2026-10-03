#!/usr/bin/env python3
"""The real built library (dist/libtakaro-conan-native.so) LD_PRELOADed into a stand-in server
executable named ConanSandboxServer-Linux-Shipping, against tests/fake_takaro.py.

The stand-in is not build 25639945, so this drives the shipped unknown-build path end to end:
config from takaro.json (fail closed when missing or broken), the pins scan refusing the build, no
hook, identify anyway, exactly one critical notice, structured errors for every action, and the
durable outbox replaying the unconfirmed notice across a forced Takaro outage.

Usage: so_test.py <library .so> <fake server executable>
"""
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fake_takaro import FakeTakaro, events_of, make_cert  # noqa: E402

IDENTITY = 'so-identity-SECRET-55aa'
REGISTRATION = 'so-registration-SECRET-66bb'
OUTAGE = float(os.environ.get('OUTAGE_SECONDS', '20'))
RESULTS = []


def check(cond, label, detail=''):
    RESULTS.append((bool(cond), label))
    print(('PASS ' if cond else 'FAIL ') + label + ('' if cond or not detail else f'  -- {detail}'), flush=True)
    if not cond:
        raise AssertionError(label + (': ' + str(detail) if detail else ''))


def eventually(fn, timeout=10, what='condition'):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = fn()
        if last:
            return last
        time.sleep(0.1)
    raise AssertionError(f'{what} did not happen within {timeout}s (last={last!r})')


class Server:
    """A stand-in server tree: <root>/ConanSandbox/{Binaries/Linux/<exe>, Saved/...}."""

    def __init__(self, root: Path, exe_src: Path, lib: Path):
        self.root = root
        self.bin = root / 'ConanSandbox/Binaries/Linux/ConanSandboxServer-Linux-Shipping'
        self.bin.parent.mkdir(parents=True, exist_ok=True)
        self.bin.write_bytes(exe_src.read_bytes())
        self.bin.chmod(0o755)
        self.saved = root / 'ConanSandbox/Saved'
        self.config = self.saved / 'Config/Takaro/takaro.json'
        self.log = self.saved / 'Logs/TakaroConanNative.log'
        self.state = self.saved / 'Takaro/state'
        self.lib = lib
        self.proc = None

    def write_config(self, text):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        self.config.write_text(text)

    def start(self, extra_env=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith('TAKARO_')}
        env.update(LD_PRELOAD=str(self.lib), TAKARO_RECONNECT_BASE_MS='500', TAKARO_RECONNECT_MAX_MS='4000')
        env.update(extra_env or {})
        self.proc = subprocess.Popen([str(self.bin)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, env=env)
        return self

    def stop(self):
        self.proc.stdin.close()  # the stand-in exits normally, so the library destructor runs
        self.proc.wait(timeout=15)
        return self.proc.returncode

    def log_text(self):
        return self.log.read_text() if self.log.exists() else ''

    def health(self):
        p = self.state / 'health.json'
        return json.loads(p.read_text()) if p.exists() else {}


def main():
    lib, exe = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix='conan-so-') as tmp:
        tmp = Path(tmp)
        key, cert = make_cert(tmp)
        fake = FakeTakaro(key, cert)
        url = f'wss://localhost:{fake.port}/'
        try:
            # 0. other processes inherit LD_PRELOAD too: the library must stay out of them
            r = subprocess.run(['/bin/sh', '-c', 'echo inert'], env=dict(os.environ, LD_PRELOAD=str(lib)),
                               capture_output=True, text=True, timeout=10)
            check(r.returncode == 0 and r.stdout.strip() == 'inert', 'a non-Conan process with LD_PRELOAD is untouched')

            # 1. fail closed
            s = Server(tmp / 'noconfig', exe, lib).start()
            time.sleep(2)
            check(s.stop() == 0 and 'connector off: not configured' in s.log_text() and not fake.conns,
                  'no takaro.json and no env: inert, no connection, reason logged', s.log_text()[-300:])
            s = Server(tmp / 'badjson', exe, lib)
            s.write_config('{"identityToken": "x", oops')
            s.start()
            time.sleep(2)
            check(s.stop() == 0 and 'failing closed' in s.log_text() and not fake.conns,
                  'malformed takaro.json: fails closed, no connection', s.log_text()[-300:])
            s = Server(tmp / 'plain', exe, lib)
            s.write_config(json.dumps({'url': 'ws://localhost:1/', 'identityToken': IDENTITY,
                                       'registrationToken': REGISTRATION}))
            s.start()
            time.sleep(2)
            check(s.stop() == 0 and 'wss://' in s.log_text() and not fake.conns,
                  'a plaintext ws:// URL is refused')

            # 2. the real thing: config from takaro.json, env overrides the name
            fake.pongs = False  # hold the notice unconfirmed for the outage replay below
            s = Server(tmp / 'main', exe, lib)
            s.write_config(json.dumps({'url': url, 'identityToken': IDENTITY, 'registrationToken': REGISTRATION,
                                       'name': 'From File', 'caFile': str(cert)}))
            s.start({'TAKARO_SERVER_NAME': 'From Env'})
            _, ident = fake.next_frame(lambda v: isinstance(v, dict) and v.get('type') == 'identify',
                                       what='identify')
            check(ident['payload'] == {'identityToken': IDENTITY, 'registrationToken': REGISTRATION,
                                       'name': 'From Env'},
                  'identifies with the takaro.json tokens; env wins over the file (name)', ident['payload'])
            eventually(lambda: events_of(fake.log), what='critical notice')
            notice = events_of(fake.log)
            check(len(notice) == 1 and notice[0]['type'] == 'log' and 'CRITICAL' in notice[0]['data']['msg']
                  and 'unsupported server build' in notice[0]['data']['msg'],
                  'unknown build: one critical notice', notice)
            log = s.log_text()
            check('NO HOOK INSTALLED' in log and 'ProcessEvent hooked' not in log,
                  'unknown build: the pins scan refused it and no hook was installed')
            check('pins: processEvent: no match' in log, 'the scan ran over the real main module', log[:600])
            r = fake.response(fake.request('testReachability', {}))
            check(r.get('payload', {}).get('connectable') is False, 'testReachability answers connectable=false', r)
            for action in ('sendMessage', 'getPlayers', 'kickPlayer'):
                r = fake.response(fake.request(action, {'message': 'hi', 'gameId': '76561198000000001'}))
                check('refused' in r.get('error', ''), f'{action} answers a structured refusal', r)

            # 3. outage: the unconfirmed notice survives and is replayed after reconnect
            first = len(fake.conns)
            fake.outage(OUTAGE)
            print(f'... forced Takaro outage for {OUTAGE:.0f}s', flush=True)
            time.sleep(OUTAGE)
            fake.pongs = True
            eventually(lambda: len(fake.conns) > first, timeout=15, what='reconnect')
            new = fake.conns[-1].index
            eventually(lambda: events_of(fake.log, new), timeout=10, what='replay')
            replay = events_of(fake.log, new)
            check(len(replay) == 1 and 'CRITICAL' in replay[0]['data']['msg'],
                  'after a 20 s outage the unconfirmed notice is replayed once on the new connection', replay)
            r = fake.response(fake.request('testReachability', {}))
            check(r.get('payload', {}).get('connectable') is False, 'requests are answered after the reconnect')
            eventually(lambda: s.health().get('outbox', {}).get('confirmedTotal') == 1, timeout=15,
                       what='notice confirmed (health.json)')
            check(True, 'the replayed notice is confirmed and leaves the durable outbox')
            rc = s.stop()
            log = s.log_text()
            check(rc == 0 and 'Takaro Conan native stopped' in log, 'clean exit through the library destructor')
            secrets = log + (s.state / 'health.json').read_text() + (s.state / 'event-outbox.json').read_text()
            check(IDENTITY not in secrets and REGISTRATION not in secrets, 'no token value in the log or state files')
            check(not fake.errors, 'peer saw no protocol errors', fake.errors)
        finally:
            fake.close()
    failed = [label for ok, label in RESULTS if not ok]
    print(f'library tests: {len(RESULTS) - len(failed)}/{len(RESULTS)} passed', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
