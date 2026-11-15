"""Hardware-free source/ABI baseline checks; Python standard library only."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from UcalNaming import inspect as inspect_naming
from UcalLayout import inspect as inspect_layout


def inspect(root: Path) -> dict:
    failures: list[str] = []
    checks: list[dict] = []
    protected = [root / name for name in ("ucal/src", "ucal/include", "manager/src")]
    vendor_names = []
    for descriptor in (root / 'backends').glob('*/backend.json'):
        metadata = json.loads(descriptor.read_text(encoding='utf-8-sig'))
        vendor_names.extend(str(metadata[key]) for key in ('vendor','backend_family') if metadata.get(key))
    patterns = ['(?-i:Vector)' if name == 'Vector' else re.escape(name) for name in vendor_names]
    vendor_words = re.compile(r"\b(?:" + '|'.join(patterns) + r")\b", re.I) if patterns else re.compile(r'(?!)')
    vendor_mentions: list[dict] = []
    for folder in protected:
        for path in folder.rglob("*"):
            if path.suffix not in {".c", ".cpp", ".h", ".hpp"}:
                continue
            relative = path.relative_to(root).as_posix()
            for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
                if vendor_words.search(line):
                    failures.append(f"Vendor implementation in framework: {relative}:{number}")
                if vendor_words.search(line):
                    vendor_mentions.append({"path": relative, "line": number, "text": line.strip()})
                match = re.match(r'\s*#include\s+"([^"]+)"', line)
                if match and (root / match[1]).is_file() and match[1].startswith(("frontends/", "backends/")):
                    failures.append(f"Framework includes optional plugin: {relative}:{number}")
                if match and folder.is_relative_to(root / 'ucal'):
                    candidates = [path.parent / match[1], root / "ucal/include" / match[1],
                                  root / "ucal/src" / match[1], root / "ucal/src/host" / match[1]]
                    if not any(p.is_file() for p in candidates):
                        failures.append(f"UCAL depends on a header outside UCAL: {relative}: {match[1]}")
    solution = (root / "Ucal.sln").read_text(encoding="utf-8-sig")
    if re.search(r'Project\([^\n]+, "(?:frontends|backends)\\', solution):
        failures.append("Core solution has a mandatory vendor project")
    checks.append({"id": "source-boundaries", "status": "failed" if failures else "passed"})

    # These vectors are frozen data, independent of the current product spelling.
    fixture = json.loads((root / "ucal/test/res/endpoint-identity-v1.json").read_text())
    namespace = bytes(fixture["namespaceBytes"])
    identity_ok = True
    for vector in fixture["vectors"]:
        digest = hashlib.sha256(namespace + vector["materialSuffix"].encode()).digest()[:16].hex()
        formatted = f"{digest[:8]}-{digest[8:12]}-{digest[12:16]}-{digest[16:20]}-{digest[20:]}"
        identity_ok &= formatted == vector["expectedEndpointId"]
    model = (root / "ucal/include/ucal/Model.hpp").read_text()
    identity_ok &= "{{0x75, 0x63, 0x61, 0x69}}" in model
    for file in ("ucal/src/host/backends/PluginBackend.cpp", "ucal/src/host/backends/j2534/J2534Backend.cpp"):
        identity_ok &= "endpointIdentityNamespaceV1.data()" in (root / file).read_text()
    if not identity_ok:
        failures.append("Version-1 endpoint identity namespace or known vectors changed")
    checks.append({"id": "frozen-endpoint-identities", "status": "passed" if identity_ok else "failed"})

    catalog = json.loads((root / "manager/test/cases.json").read_text())
    test_source = (root / "manager/test/ManagerModelTest.cpp").read_text(encoding="utf-8")
    compiled_tests = set(re.findall(r"^void ManagerModelTest::(\w+)\(", test_source, re.M))
    compiled_tests -= {"initTestCase", "cleanupTestCase"}
    actual = [case["id"] for case in catalog["cases"]]
    catalog_ok = len(set(actual)) == len(actual) == catalog["caseCount"] and set(actual) == compiled_tests
    if not catalog_ok:
        failures.append("Manager case catalog does not match the functional test implementations")
    checks.append({"id": "manager-case-catalog", "status": "passed" if catalog_ok else "failed"})

    naming = inspect_naming(root)
    failures.extend(naming["failures"])
    checks.append({"id": "ucal-naming", "status": naming["status"]})
    layout = inspect_layout(root)
    failures.extend(layout["failures"])
    checks.append({"id": "source-layout", "status": layout["status"]})
    return {"schema": "org.ucal.source-boundaries/1", "status": "failed" if failures else "passed",
            "checks": checks, "failures": failures, "remainingCompatibilityNames": vendor_mentions}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = inspect(args.root.resolve())
    payload = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "checks": result["checks"],
                      "failures": result["failures"],
                      "remainingCompatibilityNameCount": len(result["remainingCompatibilityNames"])}, ensure_ascii=False))
    return int(bool(result["failures"]))


if __name__ == "__main__":
    raise SystemExit(main())
