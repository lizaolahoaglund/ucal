"""Validate module ownership, build inputs, resources and public documentation."""
import json
from pathlib import Path
import re

MODULE_ROOTS = ('ucal', 'manager', 'frontends', 'backends')
CODE = {'.cpp','.c','.h','.hpp','.pro','.pri','.vcxproj','.py','.ps1','.psm1'}

def inspect(root):
    root = Path(root).resolve()
    failures = []
    obsolete = ('src','tests','test/manager','test/ucal','test/j2534','test/plugins',
                'test/frontends','test/backends','test/res','test/scripts','test/poc','tools/poc-launcher')
    for relative in obsolete:
        folder = root / relative
        if folder.exists() and any(p.is_file() and '__pycache__' not in p.parts for p in folder.rglob('*')):
            failures.append(f'Obsolete source ownership: {relative}')
    groups, private = [], []
    for category in ('frontends','backends'):
        for descriptor in (root/category).glob('*/plugin-package.json'):
            folder = descriptor.parent
            groups.append(folder)
            data = json.loads(descriptor.read_text(encoding='utf-8-sig'))
            if data.get('visibility') == 'private': private.append(folder)
            for asset in data.get('source_files',[]):
                if not (folder/asset['source']).is_file():
                    if asset.get('source_visibility') == 'private': continue
                    failures.append(f'Missing module asset: {descriptor}: {asset["source"]}')
            for name in data.get('backends',[]):
                if not (folder/name).is_file(): failures.append(f'Missing backend metadata: {descriptor}: {name}')
            for name in ('backend.json','frontend.json'):
                path = folder/name
                if not path.is_file(): continue
                metadata=json.loads(path.read_text(encoding='utf-8-sig'))
                if any(key in metadata for key in ('channels','endpoints','online')):
                    failures.append(f'Runtime inventory in static metadata: {path}')
                if data.get('visibility') == 'public' and {'launch', 'launcher_profiles'} & metadata.keys():
                    failures.append(f'Developer launch settings in public runtime metadata: {path}')
            launch_path = folder/'tools/launch/launcher.json'
            if (folder/'frontend.json').is_file() and launch_path.is_file():
                launch = json.loads(launch_path.read_text(encoding='utf-8-sig'))
                for key in ('plan', 'start'):
                    target = (launch_path.parent/launch.get(key, '')).resolve()
                    if not target.is_relative_to(launch_path.parent) or not target.is_file():
                        failures.append(f'Invalid module launch entry: {launch_path}: {key}')
            if (folder/'package').exists() or (folder/'tests').exists():
                failures.append(f'Redundant module directory: {folder}')
            for source in (folder/'src').rglob('*'):
                if source.suffix not in ('.cpp','.hpp','.h','.c'): continue
                for token in re.findall(r'#include\s+"([^"]+)"',source.read_text(encoding='utf-8-sig')):
                    target=(source.parent/token).resolve()
                    if target.is_relative_to(root/category) and not target.is_relative_to(folder):
                        failures.append(f'Sibling private header dependency: {source}: {token}')
            if data.get('kind')=='api' and (folder/'frontend.json').exists():
                failures.append(f'API-only module declares installed-software metadata: {folder}')
    for module in ('ucal','manager'):
        if not (root/module/'src').is_dir(): failures.append(f'Missing implementation directory: {module}')
        if not (root/module/'test').is_dir(): failures.append(f'Missing test directory: {module}')
    for base in MODULE_ROOTS:
        for resource in (root/base).rglob('*'):
            if resource.is_file() and 'res' in resource.relative_to(root/base).parts and resource.suffix in CODE:
                failures.append(f'Implementation stored as resource: {resource}')
        for project in (root/base).rglob('*'):
            if project.suffix not in ('.pro','.pri'): continue
            text=re.sub(r'\\\r?\n',' ',project.read_text(encoding='utf-8-sig'))
            inputs=[]
            for line in text.splitlines():
                match=re.search(r'(?:SOURCES|HEADERS|RESOURCES|DEF_FILE|RC_FILE|RC_ICONS)\s*\+?=\s*([^#]+)',line)
                if match: inputs.extend(match[1].split())
                inputs.extend(re.findall(r'include\(([^)]+)\)',line))
            for token in inputs:
                token=token.strip('"').replace('$$PWD',str(project.parent)).replace('$$UCAL_SOURCE_ROOT',str(root))
                if '$' in token or '(' in token or token=='\\': continue
                target=Path(token) if Path(token).is_absolute() else project.parent/token
                if not target.exists(): failures.append(f'Missing qmake input: {project}: {token}')
        for resource in (root/base).rglob('*.qrc'):
            for token in re.findall(r'<file[^>]*>([^<]+)</file>',resource.read_text(encoding='utf-8-sig')):
                if not (resource.parent/token).is_file(): failures.append(f'Missing QRC input: {resource}: {token}')
    for base in (*MODULE_ROOTS,'tools','test'):
        for script in (root/base).rglob('*'):
            if script.suffix not in ('.ps1','.psm1'): continue
            for token in re.findall(r"Join-Path\s+\$PSScriptRoot\s+'([^']+)'",script.read_text(encoding='utf-8-sig')):
                if token.endswith(('.ps1','.psm1','.vcxproj','.pro','.cpp','.h','.rc')):
                    if not (script.parent/token.replace('\\','/')).is_file():
                        failures.append(f'Missing script-local input: {script}: {token}')
    document_groups = [root.glob('*.md')] + [(root/base).rglob('*.md') for base in (*MODULE_ROOTS,'doc','tools','test')]
    for documents in document_groups:
        for source in documents:
            if any(source.is_relative_to(folder) for folder in private): continue
            for token in re.findall(r'\]\(([^\s)]+)\)',source.read_text(encoding='utf-8-sig')):
                token=token.split('#',1)[0]
                if not token or '://' in token or token.startswith(('mailto:','/')): continue
                target=(source.parent/token).resolve()
                if target.is_relative_to(root) and target.relative_to(root).parts[0].startswith('.'):
                    failures.append(f'Public document links internal record: {source}: {token}')
                elif not target.exists(): failures.append(f'Broken document link: {source}: {token}')
    discovery_doc = root/'ucal/doc/README.md'
    discovery_header = (root/'ucal/include/ucal/J2534Discovery.h').read_text(encoding='utf-8-sig')
    for line in discovery_doc.read_text(encoding='utf-8-sig').splitlines():
        if line.startswith('| J2534 discovery |'):
            for symbol in re.findall(r'`(Ucal\w+)`', line):
                if not re.search(r'\b' + re.escape(symbol) + r'\s*\(', discovery_header):
                    failures.append(f'Documented discovery export is not a function: {symbol}')
    return {'schema':'org.ucal.layout-check/2','status':'failed' if failures else 'passed',
            'sourceGroupCount':len(groups),'failures':failures}
