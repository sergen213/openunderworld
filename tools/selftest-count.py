#!/usr/bin/env python3
"""Count self-test results by each report's TOP-LEVEL verdict.

usage: tools/selftest-count.py DIR [DIR...]
  DIR holds the *.report.json files of one self-test run (ou-game --self-test-report). Prints "DIR pass / total"
  and the failing scenario names. Never count with grep for '"passed": true': step-level passes match it too.
"""
import glob
import json
import sys


def verdict(path):
    try:
        p = json.load(open(path)).get('passed')
    except (OSError, ValueError):
        return False
    return p if isinstance(p, bool) else str(p).lower() == 'true'


def main(dirs):
    if not dirs:
        sys.exit(__doc__)
    for d in dirs:
        files = sorted(glob.glob(f'{d}/*.report.json'))
        failing = [f.rsplit('/', 1)[-1][:-len('.report.json')] for f in files if not verdict(f)]
        print(f'{d} {len(files) - len(failing)} / {len(files)}')
        if failing:
            print('  failing: ' + ' '.join(failing))


if __name__ == '__main__':
    main(sys.argv[1:])
