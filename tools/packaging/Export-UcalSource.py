"""Export committed public source without internal files, private plugins or Git history."""
import argparse
import io
import json
from pathlib import Path
import subprocess
import tarfile
import fnmatch
import importlib.util

_policy_spec = importlib.util.spec_from_file_location('ucal_public_source_policy', Path(__file__).with_name('PublicSourcePolicy.py'))
_policy = importlib.util.module_from_spec(_policy_spec)
_policy_spec.loader.exec_module(_policy)


def export(root, output, revision='HEAD', archive_output=None):
    root, output = root.resolve(), output.resolve()
    git_root = Path(subprocess.check_output(
        ['git', '-C', str(root), 'rev-parse', '--show-toplevel'], text=True).strip()).resolve()
    if git_root != root:
        raise ValueError('Source export must run against the actual repository root.')
    if not output.is_relative_to(root / 'out'):
        raise ValueError('Select an output directory under the repository out directory.')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output must be new or empty.')
    if archive_output is not None:
        archive_output = archive_output.resolve()
        if not archive_output.is_relative_to(root / 'out'):
            raise ValueError('Select an archive path under the repository out directory.')
        if archive_output.exists():
            raise ValueError('Archive output must not already exist.')
    payload = subprocess.check_output(['git', '-C', str(root), 'archive', '--format=tar', revision])
    policy = _policy.load(root, revision)
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:') as archive:
        entries = archive.getmembers()
        for member in entries:
            target = (output / member.name).resolve()
            if not target.is_relative_to(output) or member.issym() or member.islnk():
                raise ValueError('Source archive contains an unsafe member.')
        # Empty containers for ignored modules carry no source and are not exported.
        members = [member for member in entries if member.isfile()]
        names = {member.name for member in members}
        for member in members:
            first = member.name.split('/')[0]
            if first in ('AGENTS.md', 'out') or (first == 'dist' and member.name != 'dist/README.md') or (
                    first.startswith('.') and first not in ('.github', '.gitignore', '.gitattributes')):
                raise ValueError('Internal content entered the public archive: ' + member.name)
            _policy.validate(member.name, archive.extractfile(member).read(), policy)
            if member.name.endswith('/plugin-package.json'):
                description = json.load(archive.extractfile(member))
                if description.get('visibility') == 'private':
                    raise ValueError('Private source group entered the public archive: ' + member.name)
                prefix = str(Path(member.name).parent).replace('\\', '/') + '/'
                for pattern in description.get('private_sources', []):
                    for name in names:
                        if name.startswith(prefix) and fnmatch.fnmatchcase(name[len(prefix):], pattern):
                            raise ValueError('Private compatibility implementation entered the public archive: ' + name)
        output.mkdir(parents=True, exist_ok=True)
        archive.extractall(output, members=members, filter='data')
        if archive_output is not None:
            archive_output.parent.mkdir(parents=True, exist_ok=True)
            # Use the same validated file set as the source tree. A raw Git
            # archive also contains empty directories of export-ignored groups.
            with tarfile.open(archive_output, mode='x', format=tarfile.PAX_FORMAT) as target:
                for member in members:
                    with archive.extractfile(member) as content:
                        target.addfile(member, content)
    return {'status': 'exported', 'revision': subprocess.check_output(
        ['git', '-C', str(root), 'rev-parse', revision], text=True).strip(),
        'fileCount': sum(member.isfile() for member in members), 'gitHistoryIncluded': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--revision', default='HEAD')
    parser.add_argument('--archive-output', type=Path)
    args = parser.parse_args()
    print(json.dumps(export(args.root, args.output, args.revision, args.archive_output)))
