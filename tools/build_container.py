#!/usr/bin/env python3
"""Build the same pinned container through either official image registry."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    config = json.loads((ROOT / 'config/build-container.json').read_text())
    registries = (config['registry'], 'docker.io/library')
    for index, registry in enumerate(registries):
        image = registry + '/' + config['image'] + '@' + config['index_digest']
        process = subprocess.Popen(
            ['docker', 'build', '--build-arg', 'SUNUEFI_BASE_IMAGE=' + image,
             '-t', 'sunuefi-builder', '-f', 'Dockerfile', '.'],
            cwd=ROOT / 'containers', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        lines = []
        for line in process.stdout:
            print(line, end='', flush=True)
            lines.append(line)
        result = process.wait()
        if result == 0:
            return
        log = ''.join(lines)
        # Do not rerun compiler/APT failures against a different registry.
        registry_failure = ('failed to resolve source metadata' in log or
                            '[internal] load metadata' in log and 'failed to authorize' in log)
        if not registry_failure or index == len(registries) - 1:
            raise SystemExit(result)
        print('Base image registry unavailable; retrying the same digest via Docker Hub.',
              flush=True)


if __name__ == '__main__':
    main()
