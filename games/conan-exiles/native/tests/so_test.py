#!/usr/bin/env python3
"""The real built library (dist/libtakaro-conan-native.so) LD_PRELOADed into a stand-in server
executable named ConanSandboxServer-Linux-Shipping, against tests/fake_takaro.py.

The stand-in is not build 25639945, so this drives the shipped unknown-build path end to end:
config from takaro.json, the pins scan refusing the build, no hook, identify anyway, exactly one
critical notice, structured errors for every action, and the durable outbox replaying the
unconfirmed notice across a forced Takaro outage.

config_reload: takaro.json while the server runs (the shipped flow). A missing file is created
from the template with a generated identity, the console (stdout) banner names it, and nothing
dials until a token is saved; a wrong token shows the refusal banner and a corrected one
reconnects; a half-saved file is ignored; a replaced file recovers from the saved copy; the
environment wins; a prior install without an identity never gets a new one.

Usage: so_test.py <library .so> <fake server executable>
"""
import json
import os
import re
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
        self.stdout = root / 'stdout.txt'
        self.lib = lib
        self.proc = None

    def write_config(self, text):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        self.config.write_text(text)

    def start(self, extra_env=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith('TAKARO_')}
        env.update(LD_PRELOAD=str(self.lib), TAKARO_RECONNECT_BASE_MS='500', TAKARO_RECONNECT_MAX_MS='4000')
        env.update(extra_env or {})
        self.out = open(self.stdout, 'ab')
        self.proc = subprocess.Popen([str(self.bin)], stdin=subprocess.PIPE, stdout=self.out,
                                     stderr=subprocess.DEVNULL, env=env)
        return self

    def console(self):
        return self.stdout.read_text() if self.stdout.exists() else ''

    def stop(self):
        self.proc.stdin.close()  # the stand-in exits normally, so the library destructor runs
        self.proc.wait(timeout=15)
        self.out.close()
        return self.proc.returncode

    def log_text(self):
        return self.log.read_text() if self.log.exists() else ''

    def health(self):
        p = self.state / 'health.json'
        return json.loads(p.read_text()) if p.exists() else {}


UUID = re.compile(r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$')


def identifies(fake, since=0):
    return [v for _, _, v in fake.log[since:] if isinstance(v, dict) and v.get('type') == 'identify']


def save(path, text):
    """Save the way an editor does: a temp file renamed over the original."""
    tmp = path.with_name(path.name + '.edit')
    tmp.write_text(text)
    os.replace(tmp, path)


def config_reload(tmp, exe, lib, fake, url, cert):
    env = {'TAKARO_WS_URL': url, 'TAKARO_CA_FILE': str(cert)}
    fake.drain()
    start_conns = len(fake.conns)

    # Fresh install, no file: created from the template, banner on stdout, nothing dialled.
    s = Server(tmp / 'fresh', exe, lib).start(env)
    eventually(lambda: 'registrationToken not set' in s.console(), what='no-token banner')
    out = s.console()
    check(str(s.config) in out and 'no restart needed' in out and out.count('*' * 73) == 2,
          'fresh install: the no-token banner names the exact file', out)
    created = json.loads(s.config.read_text())
    check(UUID.match(created['identityToken']) and created['registrationToken'] == ''
          and created['name'] == 'Conan Exiles (' + created['identityToken'][:8] + ')'
          and (s.config.stat().st_mode & 0o777) == 0o600,
          'the missing takaro.json is created with a generated identity and a unique name', created)
    identity = created['identityToken']
    time.sleep(6)
    check(len(fake.conns) == start_conns, 'no token: never dials Takaro')
    pid = s.proc.pid
    before = len(fake.log)
    created['registrationToken'] = REGISTRATION
    save(s.config, json.dumps(created, indent=2))
    _, ident = fake.next_frame(lambda v: isinstance(v, dict) and v.get('type') == 'identify', timeout=15,
                               what='identify after the token was saved')
    check(ident['payload'] == {'identityToken': identity, 'registrationToken': REGISTRATION,
                               'name': 'Conan Exiles (' + identity[:8] + ')'},
          'token saved while running: identifies with the generated identity, no restart', ident['payload'])
    eventually(lambda: 'connected to Takaro as' in s.console(), what='connected line')
    saved = json.loads((s.state / 'saved-settings.json').read_text())
    check(saved['identityToken'] == identity and saved['registrationToken'] == REGISTRATION,
          'after identify the saved copy holds the identity and the accepted token')

    # Wrong token saved while running: refusal banner; corrected token reconnects at once.
    fake.reject_identify = True
    conns = len(fake.conns)
    created['registrationToken'] = 'wrong-token-1234'
    save(s.config, json.dumps(created, indent=2))
    eventually(lambda: 'Takaro refused this server' in s.console(), timeout=15, what='refusal banner')
    check(len(fake.conns) > conns and 'bad token' in s.console() and 'Check registrationToken in ' + str(s.config)
          in s.console(), 'wrong token: refusal banner names the file')
    fake.reject_identify = False
    since = len(fake.log)
    t0 = time.monotonic()
    created['registrationToken'] = REGISTRATION
    save(s.config, json.dumps(created, indent=2))
    eventually(lambda: [i for i in identifies(fake, since) if i['payload']['registrationToken'] == REGISTRATION],
               timeout=15, what='identify with the corrected token')
    took = time.monotonic() - t0
    check(took < 10,
          f'corrected token reconnects within {took:.1f}s, same process', s.console()[-400:])
    check(s.proc.pid == pid and s.proc.poll() is None, 'no restart')

    # A half-saved file is ignored; the settings in use stay.
    since = len(fake.log)
    save(s.config, '{"registrationToken": "' + REGISTRATION[:5])
    eventually(lambda: 'keeping the current settings' in s.console(), timeout=15, what='half-saved warning')
    time.sleep(2)
    check(not identifies(fake, since), 'half-saved file: no reconnect')

    # The shipped file copied over takaro.json (a mistaken upgrade): nothing changes, the identity
    # is written back from the saved copy, the token is not.
    template = (Path(__file__).resolve().parent.parent / 'takaro.json').read_text()
    save(s.config, template)
    eventually(lambda: identity in s.config.read_text(), timeout=15, what='identity written back')
    time.sleep(1)
    check(not identifies(fake, since) and REGISTRATION not in s.config.read_text(),
          'replaced takaro.json: identity restored from the saved copy, no reconnect, token not copied in')
    check(s.stop() == 0, 'clean stop')
    secrets = s.log_text() + s.console()
    check(REGISTRATION not in secrets and 'wrong-token-1234' not in secrets, 'no token in the log or console')

    # Restart of that install: same identity, token from the saved copy.
    since = len(fake.log)
    s.start(env)
    eventually(lambda: identifies(fake, since), timeout=15, what='identify after restart')
    p = identifies(fake, since)[-1]['payload']
    check(p['identityToken'] == identity and p['registrationToken'] == REGISTRATION,
          'restart after the replaced file: same identity, token from the saved copy')
    check(s.stop() == 0, 'clean stop')

    # Environment wins: file edits to the token do not reconnect.
    s = Server(tmp / 'envwins', exe, lib)
    s.write_config(json.dumps({'identityToken': 'file-identity-1', 'registrationToken': 'file-token-1'}))
    since = len(fake.log)
    s.start(dict(env, TAKARO_REGISTRATION_TOKEN=REGISTRATION, TAKARO_IDENTITY_TOKEN=IDENTITY))
    eventually(lambda: identifies(fake, since), timeout=15, what='env identify')
    p = identifies(fake, since)[-1]['payload']
    check(p['identityToken'] == IDENTITY and p['registrationToken'] == REGISTRATION, 'environment wins over the file')
    save(s.config, json.dumps({'identityToken': 'file-identity-1', 'registrationToken': 'file-token-2'}))
    time.sleep(8)
    check(len(identifies(fake, since)) == 1, 'a file edit hidden by the environment does not reconnect')
    check(s.stop() == 0 and not (s.state / 'saved-settings.json').exists(),
          'an install configured by the environment writes no saved copy')

    # A prior install (state dir from an older connector) whose takaro.json lost its identity.
    s = Server(tmp / 'prior', exe, lib)
    s.state.mkdir(parents=True)
    (s.state / 'event-outbox.json').write_text('{}')
    s.write_config(json.dumps({'registrationToken': REGISTRATION}))
    conns = len(fake.conns)
    s.start(env)
    eventually(lambda: 'identityToken is empty' in s.console(), what='no-identity banner')
    time.sleep(3)
    check(len(fake.conns) == conns and 'identityToken' not in json.loads(s.config.read_text()),
          'prior install without an identity: banner, no connection, no new identity')
    check(s.stop() == 0, 'clean stop')


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

            # 1. held, not connected
            s = Server(tmp / 'badjson', exe, lib)
            s.write_config('{"identityToken": "x", oops')
            s.start()
            time.sleep(2)
            check(s.stop() == 0 and 'not valid JSON' in s.console() and not fake.conns,
                  'malformed takaro.json: console banner, no connection', s.console()[-300:])
            s = Server(tmp / 'plain', exe, lib)
            s.write_config(json.dumps({'url': 'ws://localhost:1/', 'identityToken': IDENTITY,
                                       'registrationToken': REGISTRATION}))
            s.start()
            time.sleep(2)
            check(s.stop() == 0 and 'wss://' in s.console() and not fake.conns,
                  'a plaintext ws:// URL is refused')
            s = Server(tmp / 'disabled', exe, lib).start({'TAKARO_CONAN_NATIVE_DISABLE': '1'})
            time.sleep(2)
            check(s.stop() == 0 and 'connector off: disabled' in s.log_text() and not s.config.exists()
                  and not fake.conns, 'TAKARO_CONAN_NATIVE_DISABLE=1: inert, writes nothing')

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

            config_reload(tmp, exe, lib, fake, url, cert)
            check(not fake.errors, 'peer saw no protocol errors during config_reload', fake.errors)
        finally:
            fake.close()
    failed = [label for ok, label in RESULTS if not ok]
    print(f'library tests: {len(RESULTS) - len(failed)}/{len(RESULTS)} passed', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
