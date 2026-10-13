#!/usr/bin/env python3
"""Check compiled payloads for Keenetic-only policy and interface RCI code."""
import argparse
from pathlib import Path

BACKEND = [
    b'/api/v1/keenetic/policy', b'/rci/show/ip/policy', b'keeneticPolicy',
    b'POLICY_RULES', b'/rci/show/interface', b'http://127.0.0.1:79',
    b'failed to load interface aliases',
]
FRONTEND = [
    b'/keenetic/policy', b'Use policy based filter', b'keenetic-policy-enabled',
    'Политика Keenetic'.encode(),
]


def verify(path: Path, needles: list[bytes], enabled: bool) -> None:
    files = [path] if path.is_file() else [p for p in path.rglob('*') if p.is_file()]
    if not files:
        raise SystemExit(f'Missing payload: {path}')
    payload = b''.join(p.read_bytes() for p in files)
    for needle in needles:
        if (needle in payload) != enabled:
            raise SystemExit(f'{path}: {needle!r} must be {"present" if enabled else "absent"}')
    # Native unstripped binaries must not retain exported RCI helpers either.
    if not enabled and b'mt_kn_' in payload:
        raise SystemExit(f'{path}: Keenetic symbols must be absent')
    print(f'PASS {"Keenetic feature present" if enabled else "no Keenetic integration"}: {path}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    checks = (
        ('generic-binary', BACKEND, False), ('keenetic-binary', BACKEND, True),
        ('generic-frontend', FRONTEND, False), ('keenetic-frontend', FRONTEND, True),
    )
    for name, _, _ in checks:
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    if not any(vars(args).values()):
        parser.error('provide at least one compiled binary or frontend payload')
    for name, needles, enabled in checks:
        path = getattr(args, name.replace('-', '_'))
        if path is not None:
            verify(path, needles, enabled)
