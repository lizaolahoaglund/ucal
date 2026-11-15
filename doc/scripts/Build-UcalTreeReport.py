"""Export complete source/runtime trees as local HTML and UTF-8 text."""
import argparse
import html
import os
from pathlib import Path
import subprocess

ROLES = {'ucal': 'Vendor-neutral foundation', 'ucal/include/ucal': 'Public interfaces', 'ucal/src': 'Core, transport, Broker and shared backend templates', 'ucal/test': 'Core regression and contract tests', 'ucal/tools': 'Shared API export and deployment tools', 'manager': 'Qt Manager', 'manager/src': 'User interface and control logic', 'manager/test': 'Manager regression and package validation', 'manager/res': 'Icons and demonstration resources', 'frontends': 'Independent API and application frontends', 'backends': 'Independent hardware backends', 'test': 'Integration, source boundaries and regression runners', 'tools': 'Build, packaging and launch tools', 'tools/launcher': 'Application launch using module metadata', 'doc': 'Architecture and baseline requirements', 'dist': 'Generated portable package', 'runtime': 'Runtime hosts and loaders'}


def source_files(root):
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '--cached', '--others', '--exclude-standard'],
                            capture_output=True, text=True, check=False)
    if result.returncode == 0:
        files = result.stdout.splitlines()
    else:
        files = [p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()]
    return sorted(set(name for name in files if not name.startswith(('out/', 'dist/', '.git/'))
                      and (root/name).is_file()))

def tree(files):
    node = {}
    for name in files:
        current = node
        parts = name.split('/')
        for part in parts[:-1]:
            current = current.setdefault(part, {})
        current[parts[-1]] = None
    return node

def nodes(node):
    priority = {'src': 0, 'ucal': 0, 'test': 1, 'manager': 1, 'doc': 2, 'frontends': 2, 'tools': 3, 'backends': 3, 'apis': 4}
    return sorted(node.items(), key=lambda item: (item[1] is None, priority.get(item[0], 10), item[0].casefold()))

def plain(node, prefix=''):
    lines = []
    children = nodes(node)
    for index, (name, child) in enumerate(children):
        last = index == len(children)-1
        lines.append(prefix+('└── ' if last else '├── ')+name+('/' if child is not None else ''))
        if child is not None:
            lines.extend(plain(child, prefix+('    ' if last else '│   ')))
    return lines

def markup(node, root, output_parent, parent='', depth=0):
    rows = []
    for name, child in nodes(node):
        relative = (parent+'/'+name).lstrip('/')
        if child is None:
            href = os.path.relpath(root/relative, output_parent).replace('\\', '/')
            rows.append('<li class="file"><a href="'+html.escape(href)+'">'+html.escape(name)+'</a></li>')
        else:
            note = ROLES.get(relative, '')
            rows.append('<li><details'+(' open' if depth < 3 else '')+'><summary>'+html.escape(name)+
                        '/ <small>'+html.escape(note)+'</small></summary><ul>'+markup(child, root, output_parent, relative, depth+1)+'</ul></details></li>')
    return ''.join(rows)

def build(root, dist, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    source = source_files(root)
    runtime = sorted(p.relative_to(dist).as_posix() for p in dist.rglob('*') if p.is_file())
    trees = [('Source tree', root, source), ('Portable package' if dist.name == 'dist' else 'Runtime directory (' + dist.name + ')', dist, runtime)]
    sections = []
    for title, base, files in trees:
        model = tree(files)
        text_name = 'SourceTree.txt' if base == root else 'DistTree.txt'
        (output.parent/text_name).write_text('\n'.join(plain(model))+'\n', encoding='utf-8')
        sections.append('<section><h2>'+title+'</h2><p>'+str(len(files))+' files · <a href="'+text_name+'">Download the complete text tree</a></p><ul class="tree">'+markup(model, base, output.parent)+'</ul></section>')
    body = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>UCAL project directory tree</title><style>
body{margin:0;background:#f4f7fc;color:#172c50;font:15px/1.7 "Microsoft YaHei","Segoe UI",sans-serif}main{max-width:1280px;margin:32px auto;padding:0 24px 40px}header{background:#0c244c;color:#fff;border-radius:14px;padding:24px 30px}h1{margin:0;font-size:27px}header p{color:#c0d5f1}.toolbar{position:sticky;top:0;padding:14px 0;background:#f4f7fc;display:flex;align-items:center;gap:12px;z-index:1}button{padding:8px 15px;border:1px solid #bccde6;background:white;color:#154981;border-radius:7px;cursor:pointer}section{background:#fff;padding:24px;border-radius:12px;border:1px solid #d9e3f1;margin:16px 0}h2{margin:0 0 8px}a{color:#195eb5;text-decoration:none}a:hover{text-decoration:underline}.tree,.tree ul{list-style:none;padding-left:22px}.tree{padding-left:0;font:14px/1.85 Consolas,"Microsoft YaHei",monospace}.tree ul{border-left:1px solid #e1e8f2;margin-left:6px}summary{cursor:pointer;color:#153d70}summary small{font-family:"Microsoft YaHei",sans-serif;color:#6a7d97;font-size:12px;margin-left:10px}.file{padding-left:17px;color:#445}.hide-files .file{display:none}code{background:#e9eff8;padding:2px 5px;border-radius:3px}
</style><body class="hide-files"><main><header><h1>UCAL project directory tree</h1><p>The source tree lists tracked and unignored files. The selected runtime directory is listed separately.</p></header><div class="toolbar"><button onclick="document.querySelectorAll('details').forEach(x=>x.open=true)">Expand all</button><button onclick="document.querySelectorAll('details').forEach(x=>x.open=false)">Collapse all</button><label><input type="checkbox" onchange="document.body.classList.toggle('hide-files',!this.checked)"> Show files</label></div>CONTENT</main></body></html>'''
    output.write_text(body.replace('CONTENT', ''.join(sections)), encoding='utf-8')
    return {'sourceFiles': len(source), 'runtimeFiles': len(runtime)}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--dist', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    print(build(root, (args.dist or root/'dist').resolve(), args.output.resolve()))
