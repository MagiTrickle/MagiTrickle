#!/usr/bin/env python3
"""Exercise root Makefile target/stamp isolation with an explicit fake compiler."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='mt-frontend-variants-') as tmp:
    root = Path(tmp)
    shutil.copy(REPO / 'Makefile', root / 'Makefile')
    front = root / 'src/frontend'
    (front / 'src').mkdir(parents=True)
    for name in ('package.json', 'package-lock.json', 'vite.config.ts', 'tsconfig.json'):
        (front / name).write_text('{}\n')
    tools = root / 'test-bin'
    tools.mkdir()
    npm = tools / 'npm'
    npm.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
if sys.argv[1:] == ['install']: sys.exit(0)
assert sys.argv[1:4] == ['run', 'build', '--'], sys.argv
out = Path(sys.argv[sys.argv.index('--outDir') + 1])
out.mkdir(parents=True, exist_ok=True)
(out / 'index.html').write_text('KN=' + os.environ['VITE_ENTWARE_KN'])
with Path(os.environ['BUILD_LOG']).open('a') as log: log.write(str(out) + '\\n')
''')
    npm.chmod(0o755)
    log = root / 'builds.log'
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
               BUILD_LOG=str(log), ENTWARE_KN='1')
    def build(platform, target, shared=False):
        out = root / 'shared' if shared else root / '.build' / f'{platform}_{target}' / 'frontend'
        args = ['make', 'build_frontend', f'PLATFORM={platform}', f'TARGET={target}', 'PKG_VERSION=1.2.3']
        if shared: args.append(f'FRONTEND_OUT_DIR={out}')
        result = subprocess.run(args, cwd=root, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        expected = '1' if platform == 'entware' and target.endswith('_kn') else '0'
        assert (out / 'index.html').read_text() == 'KN=' + expected
        return out
    kn = build('entware', 'aarch64-3.10_kn')
    generic = build('entware', 'aarch64-3.10')
    build('openwrt', 'x86_64')
    build('entware', 'aarch64-3.10_kn')
    assert len(log.read_text().splitlines()) == 3, 'unchanged target must reuse its own bundle'
    assert kn != generic and (kn / 'index.html').read_text() == 'KN=1'
    build('entware', 'aarch64-3.10_kn', True)
    build('entware', 'aarch64-3.10', True)
    build('entware', 'aarch64-3.10_kn', True)
    assert len(log.read_text().splitlines()) == 6, 'shared override must rebuild after variant changed'
    build('entware', 'aarch64-3.10_kn', True)
    assert len(log.read_text().splitlines()) == 6, 'identical shared output should remain incremental'
    (root / 'shared/index.html').unlink()
    build('entware', 'aarch64-3.10_kn', True)
    assert len(log.read_text().splitlines()) == 7, 'removed output must be rebuilt'
print('PASS target-specific frontend, inherited flag isolation, shared output invalidation and incremental rebuilds')
