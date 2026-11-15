"""Run the source-layout and public-document contract checks."""
import argparse
import json
from pathlib import Path
from UcalLayout import inspect

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    result = inspect(args.root)
    print(json.dumps(result, ensure_ascii=False))
    raise SystemExit(bool(result['failures']))
