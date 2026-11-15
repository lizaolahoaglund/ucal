"""Audit original Ucal C/C++ names; Python standard library only."""
import argparse
import json
from pathlib import Path
from UcalNaming import inspect

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--output', type=Path)
args = parser.parse_args()
result = inspect(args.root.resolve())
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(json.dumps({key: value for key, value in result.items() if key != 'standardIdentifiers'}, ensure_ascii=False))
raise SystemExit(0 if result['status'] == 'passed' else 1)
