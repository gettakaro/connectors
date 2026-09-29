#!/usr/bin/env python3
"""One-shot legacy JS versus pinned PCRE2 grammar proof before native cutover.

The native server never executes Node. This utility runs while the old sidecar
toolchain is still present; a custom regex without a matching fixture blocks
cutover instead of claiming untested compatibility.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

KEYS = (
    'VEIN_LOG_LOGIN_RE', 'VEIN_LOG_JOIN_RE', 'VEIN_LOG_LEAVE_RE',
    'VEIN_LOG_CHAT_RE', 'VEIN_LOG_READY_RE',
)
NODE_SCRIPT = r'''
const fs = require('fs');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const out = {};
for (const [key, source] of Object.entries(input.patterns)) {
  let regex;
  try { regex = new RegExp(source, 'i'); }
  catch (error) { out[key] = {error: String(error)}; continue; }
  out[key] = {rows: input.fixtures[key].map(line => {
    const match = regex.exec(line);
    if (!match) return {matched: false, captures: [], named: {}};
    const named = {};
    for (const [name, value] of Object.entries(match.groups || {})) named[name] = value === undefined ? null : value;
    return {matched: true, full: match[0],
            captures: Array.from(match).slice(1).map(value => value === undefined ? null : value),
            named};
  })};
}
process.stdout.write(JSON.stringify(out));
'''


def canonical_sha(value):
    raw = json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    return hashlib.sha256(raw).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True, help='pinned PCRE2 native_log_probe binary')
    parser.add_argument('--fixtures', type=Path, default=Path(__file__).resolve().parents[1] /
                        'mod/tests/native_log_legacy_fixtures.json')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--node', default='node', help='existing legacy sidecar Node executable')
    parser.add_argument('--sidecar-container', help='read live overrides and run legacy Node in this container')
    args = parser.parse_args()
    if args.sidecar_container:
        try:
            inspected = subprocess.run(['docker', 'inspect', '--format', '{{json .Config.Env}}',
                                        args.sidecar_container], text=True, capture_output=True,
                                       check=True, timeout=15)
            container_env = dict(entry.split('=', 1) for entry in json.loads(inspected.stdout) if '=' in entry)
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            parser.error('cannot inspect live sidecar environment: ' + str(error)[:200])
        patterns = {key: container_env[key] for key in KEYS if container_env.get(key, '').strip()}
    else:
        patterns = {key: os.environ[key] for key in KEYS if os.environ.get(key, '').strip()}
    fixture_bytes = args.fixtures.read_bytes()
    fixture_sha = hashlib.sha256(fixture_bytes).hexdigest()
    report = {
        'version': 1,
        'generatedAtUtc': dt.datetime.now(dt.timezone.utc).isoformat().replace('+00:00', 'Z'),
        'status': 'none' if not patterns else 'fail',
        'configuredKeys': sorted(patterns),
        'configSha256': canonical_sha(patterns),
        'fixtureSha256': fixture_sha,
        'keys': {},
    }
    if patterns:
        fixtures = json.loads(fixture_bytes)
        for key in patterns:
            lines = fixtures.get(key)
            if not isinstance(lines, list) or not all(isinstance(line, str) for line in lines):
                report['keys'][key] = {'status': 'fail', 'matchedFixtures': 0,
                                       'fixtureCount': 0, 'reason': 'fixture list missing or invalid'}
        if not report['keys']:
            # Runtime parsers trim configured wrappers; report identity remains
            # bound to the exact raw values inspected from the live sidecar.
            request = json.dumps({'patterns': {key: value.strip() for key, value in patterns.items()},
                                  'fixtures': fixtures}, ensure_ascii=False)
            try:
                node_command = (['docker', 'exec', '-i', args.sidecar_container, 'node', '-e', NODE_SCRIPT]
                                if args.sidecar_container else [args.node, '-e', NODE_SCRIPT])
                js = subprocess.run(node_command, input=request, text=True,
                                    capture_output=True, check=True, timeout=30)
                native = subprocess.run([str(args.probe)], input=request, text=True,
                                        capture_output=True, check=True, timeout=30)
                legacy_result = json.loads(js.stdout)
                native_result = json.loads(native.stdout)
            except (OSError, subprocess.SubprocessError, ValueError) as error:
                for key in patterns:
                    report['keys'][key] = {'status': 'fail', 'matchedFixtures': 0,
                                           'fixtureCount': len(fixtures[key]),
                                           'reason': 'probe unavailable or failed: ' + str(error)[:200]}
            else:
                for key in patterns:
                    left = legacy_result.get(key, {})
                    right = native_result.get(key, {})
                    reason = ''
                    if left.get('error'):
                        reason = 'legacy JavaScript rejected configured expression'
                    elif right.get('error'):
                        reason = 'pinned PCRE2 rejected or limited configured expression'
                    elif len(left.get('rows', [])) != len(fixtures[key]) or len(right.get('rows', [])) != len(fixtures[key]):
                        reason = 'probe returned incomplete fixture rows'
                    else:
                        differences = [i for i, (a, b) in enumerate(zip(left['rows'], right['rows'])) if a != b]
                        if differences:
                            reason = 'capture mismatch on fixture indexes ' + ','.join(map(str, differences))
                    matched = sum(1 for row in left.get('rows', []) if row.get('matched'))
                    if not reason and matched == 0:
                        reason = 'configured expression matched no legacy fixture; add observed lines'
                    report['keys'][key] = {
                        'status': 'fail' if reason else 'pass',
                        'matchedFixtures': matched,
                        'fixtureCount': len(fixtures[key]),
                        'reason': reason,
                    }
        if all(item['status'] == 'pass' for item in report['keys'].values()) and len(report['keys']) == len(patterns):
            report['status'] = 'pass'
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(f"native log grammar: {report['status']} ({len(patterns)} configured keys); report {args.report}")
    return 0 if report['status'] in ('pass', 'none') else 1


if __name__ == '__main__':
    sys.exit(main())
