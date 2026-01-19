import os
import subprocess
from typing import List

from SCons.Script import Import  # type: ignore
Import('env')

# Helper: run a command and return stdout or ''
def _run(cmd: List[str]) -> str:
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL).decode('utf-8', errors='ignore').strip()
        return out
    except Exception:
        return ''

# Derive version from git
commit = _run(['git', 'rev-parse', '--short', 'HEAD']) or 'unknown'
# Prefer annotated tag, fall back to describe or commit
ver = _run(['git', 'describe', '--tags', '--dirty', '--always']) or commit

# Allow override via env vars if needed
commit = os.environ.get('BRUCE_GIT_COMMIT', commit)
ver = os.environ.get('BRUCE_VERSION', ver)

# Inject into preprocessor defines (quoted strings)
cppdefs = env.get('CPPDEFINES', [])
cppdefs += [
    ("GIT_COMMIT_HASH", f'"{commit}"'),
    ("BRUCE_VERSION", f'"{ver}"'),
]
env.Replace(CPPDEFINES=cppdefs)