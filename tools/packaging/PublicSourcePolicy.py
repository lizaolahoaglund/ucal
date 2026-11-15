"""Read committed, export-excluded publication rules without embedding private terms."""
import json
import re
import subprocess


def load(root, revision):
    paths = subprocess.check_output(
        ['git', '-C', str(root), 'ls-tree', '-rz', '--name-only', revision]).decode('utf-8').split('\0')
    policies = []
    for path in paths:
        if not re.fullmatch(r'\.[^/]+/public-source-policy\.json', path):
            continue
        payload = subprocess.check_output(['git', '-C', str(root), 'show', f'{revision}:{path}'])
        if len(payload) > 65536:
            raise ValueError('Publication policy is too large.')
        policy = json.loads(payload)
        if policy.get('schema') != 'org.ucal.public-source-policy/1':
            raise ValueError('Unsupported publication policy.')
        expressions = []
        for key in ('blocked_terms', 'blocked_patterns'):
            values = policy.get(key, [])
            if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
                raise ValueError('Invalid publication policy rules.')
            for value in values:
                expressions.append(re.compile(re.escape(value) if key == 'blocked_terms' else value, re.IGNORECASE))
        policies.extend(expressions)
    return policies


def validate(name, payload, expressions):
    if not expressions:
        return
    representations = [name, payload.decode('utf-8', errors='ignore')]
    # Inspect binary strings as well, without dumping private matches into logs.
    if b'\0' in payload:
        representations.extend(payload.decode(encoding, errors='ignore') for encoding in ('utf-16-le', 'utf-16-be'))
    if any(expression.search(value) for expression in expressions for value in representations):
        raise ValueError('Forbidden public source content: ' + name)
