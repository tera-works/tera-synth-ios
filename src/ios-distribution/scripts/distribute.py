"""Unsigned iOS packaging and release-backed AltStore source. Python stdlib only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import zipfile


def run(*args, capture=False):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.check_output(list(map(str, args)), text=True) if capture else subprocess.check_call(list(map(str, args)))


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def number(value):
    require(bool(re.fullmatch(r'\d+(\.\d+){0,2}', str(value))), f'Invalid numeric version: {value}')
    parts = tuple(map(int, str(value).split('.')))
    return parts + (0,) * (3 - len(parts))


def inspect_ipa(path):
    with zipfile.ZipFile(path) as z:
        require(z.testzip() is None, 'Corrupt IPA')
        names = z.namelist()
        require(all(not n.startswith('/') and '..' not in n.split('/') for n in names), 'Unsafe ZIP path')
        roots = [n for n in names if re.fullmatch(r'Payload/[^/]+\.app/Info.plist', n)]
        require(len(roots) == 1, 'Expected exactly one Payload/*.app/Info.plist')
        info = plistlib.loads(z.read(roots[0]))
        for k in ('CFBundleIdentifier', 'CFBundleShortVersionString', 'CFBundleVersion', 'MinimumOSVersion', 'CFBundleExecutable'):
            require(bool(info.get(k)), f'Missing {k}')
        number(info['CFBundleShortVersionString'])
        number(info['CFBundleVersion'])
        require(info.get('CFBundleSupportedPlatforms') == ['iPhoneOS'], 'Not an iOS device build')
        require(roots[0].removesuffix('Info.plist') + info['CFBundleExecutable'] in names, 'Missing executable')
        require(not any('/_CodeSignature/' in n or n.endswith('/embedded.mobileprovision') for n in names), 'Signed content found')
        return info


def build(c):
    require(os.sys.platform == 'darwin', 'build requires macOS and Xcode; use GitHub Actions')
    if c.get('xcodePath'):
        os.environ['DEVELOPER_DIR'] = str(Path(c['xcodePath']) / 'Contents/Developer')
    run('xcodebuild', '-version')
    project = c.get('project')
    if not project:
        found = [p for p in Path('.').rglob('*.xcworkspace') if '.xcodeproj' not in str(p) and not any(x in p.parts for x in ('Pods', '.ios-build', '.git'))]
        if not found:
            found = [p for p in Path('.').rglob('*.xcodeproj') if not any(x in p.parts for x in ('Pods', '.ios-build', '.git'))]
        require(len(found) == 1, 'Set project: discovery is ambiguous or empty')
        project = str(found[0])
    require(Path(project).exists(), f'Project not found: {project}')
    base = ['xcodebuild', '-workspace' if project.endswith('.xcworkspace') else '-project', project]
    listing = json.loads(run(*base, '-list', '-json', capture=True))
    write('.ios-build/schemes.json', listing)
    schemes = next(iter(listing.values())).get('schemes', [])
    scheme = c.get('scheme') or (schemes[0] if len(schemes) == 1 else None)
    require(scheme in schemes, 'Set a shared application scheme in config')
    base += ['-scheme', scheme, '-configuration', 'Release', '-sdk', 'iphoneos', '-destination', 'generic/platform=iOS', '-derivedDataPath', '.ios-build/DerivedData', 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY=']
    run(*base, '-resolvePackageDependencies')
    settings = json.loads(run(*base, '-showBuildSettings', '-json', capture=True))
    write('.ios-build/settings.json', settings)
    apps = [s for s in settings if s['buildSettings'].get('WRAPPER_EXTENSION') == 'app' and (not c.get('target') or s['target'] == c['target'])]
    require(len(apps) == 1, 'Set target: expected exactly one application target')
    entitlements = set()
    for s in settings:
        b = s['buildSettings']
        ent = b.get('CODE_SIGN_ENTITLEMENTS')
        if ent:
            p = Path(ent) if Path(ent).is_absolute() else Path(b['SRCROOT']) / ent
            require(p.is_file(), f'Cannot audit entitlements: {p}')
            entitlements.update(plistlib.loads(p.read_bytes()))
    require(entitlements <= set(c.get('reviewedEntitlements', [])), 'Review free-account compatibility before acknowledging entitlements: ' + ', '.join(sorted(entitlements)))
    run(*base, 'build')
    b = apps[0]['buildSettings']
    app = Path(b['TARGET_BUILD_DIR']) / b['FULL_PRODUCT_NAME']
    require(app.is_dir(), f'Built app not found: {app}')
    dist = Path('dist')
    require(not dist.exists(), 'dist already exists; remove previous generated output first')
    dist.mkdir()
    payload = Path('.ios-build/package/Payload')
    require(not payload.exists(), 'Package directory already exists')
    payload.mkdir(parents=True)
    run('ditto', app, payload / app.name)
    ipa = dist / 'app.ipa'
    run('ditto', '-c', '-k', '--keepParent', '--norsrc', payload, ipa.resolve())
    info = inspect_ipa(ipa)
    privacy = {}
    for p in app.rglob('Info.plist'):
        for k, v in plistlib.loads(p.read_bytes()).items():
            if 'UsageDescription' in k:
                require(k not in privacy or privacy[k] == v, f'Conflicting privacy descriptions: {k}')
                privacy[k] = v
    icon = c.get('icon')
    if not icon:
        icons = list(Path('.').glob('**/*.appiconset/Contents.json'))
        candidates = []
        for contents in icons:
            if '.ios-build' in contents.parts:
                continue
            for image in read(contents).get('images', []):
                if image.get('filename') and image.get('size') == '1024x1024':
                    candidates.append(contents.parent / image['filename'])
        require(len(set(candidates)) == 1, 'Set icon to an existing PNG: discovery ambiguous or empty')
        icon = candidates[0]
    require(Path(icon).read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), 'icon must be PNG')
    shutil.copyfile(icon, dist / 'icon.png')
    manifest = {
        'name': info.get('CFBundleDisplayName') or info.get('CFBundleName'),
        'bundleIdentifier': info['CFBundleIdentifier'],
        'developerName': c['developerName'], 'localizedDescription': c['description'],
        'appPermissions': {'entitlements': sorted(entitlements), 'privacy': privacy},
        'versions': [{'version': info['CFBundleShortVersionString'], 'buildVersion': info['CFBundleVersion'],
                      'minOSVersion': info['MinimumOSVersion'], 'size': ipa.stat().st_size}],
        '_sha256': hashlib.sha256(ipa.read_bytes()).hexdigest()
    }
    write(dist / 'manifest.json', manifest)


def gh(*args):
    return run('gh', *args, capture=True)


def releases(repo):
    # API pagination: do not silently discard older versions after 100 releases.
    pages = json.loads(gh('api', '--paginate', '--slurp', f'repos/{repo}/releases?per_page=100'))
    return [r for page in pages for r in page if not r['draft'] and not r['prerelease']]


def manifests(repo):
    result = []
    for release in releases(repo):
        assets = {a['name']: a for a in release['assets']}
        if 'manifest.json' not in assets:
            continue
        m = json.loads(gh('api', '-H', 'Accept: application/octet-stream', f"repos/{repo}/releases/assets/{assets['manifest.json']['id']}"))
        require('app.ipa' in assets and 'icon.png' in assets, 'Incomplete release: ' + release['tag_name'])
        v = m['versions'][0]
        require(v['size'] == assets['app.ipa']['size'], 'Release IPA size mismatch')
        v['downloadURL'] = assets['app.ipa']['browser_download_url']
        v['date'] = release['published_at']
        v['localizedDescription'] = release.get('body') or ''
        m['iconURL'] = assets['icon.png']['browser_download_url']
        result.append(m)
    return result


def assemble(name, items):
    apps = {}
    for m in sorted(items, key=lambda m: (number(m['versions'][0]['version']), number(m['versions'][0]['buildVersion'])), reverse=True):
        bid = m['bundleIdentifier']
        v = m['versions'][0]
        if bid not in apps:
            apps[bid] = {k: val for k, val in m.items() if not k.startswith('_') and k != 'versions'}
            apps[bid]['versions'] = []
        history = apps[bid]['versions']
        require(not any(x['version'] == v['version'] and x['buildVersion'] == v['buildVersion'] for x in history), f'Duplicate release: {bid}')
        history.append(v)
    return {'name': name, 'apps': list(apps.values()), 'news': []}


def publish(c, repo):
    tag = os.environ.get('GITHUB_REF_NAME', '')
    require(bool(re.fullmatch(r'v\d+\.\d+\.\d+', tag)), 'Release tag must be vMAJOR.MINOR.PATCH')
    m = read('dist/manifest.json')
    info = inspect_ipa('dist/app.ipa')
    v = m['versions'][0]
    require(info['CFBundleIdentifier'] == m['bundleIdentifier'] and info['CFBundleVersion'] == v['buildVersion'] and info['CFBundleShortVersionString'] == v['version'], 'IPA metadata mismatch')
    require(hashlib.sha256(Path('dist/app.ipa').read_bytes()).hexdigest() == m['_sha256'], 'IPA hash mismatch')
    require(tag[1:] == v['version'], 'Tag and IPA version differ')
    history = manifests(repo)
    for old in history:
        if old['bundleIdentifier'] == m['bundleIdentifier']:
            prev = old['versions'][0]
            require((number(v['version']), number(v['buildVersion'])) > (number(prev['version']), number(prev['buildVersion'])), 'Version must be newer than every published version')
            require(number(v['buildVersion']) > number(prev['buildVersion']), 'Build number must increase across releases')
    # Existing tags (including drafts) are intentionally never overwritten.
    gh('release', 'create', tag, 'dist/app.ipa', 'dist/icon.png', 'dist/manifest.json', '--repo', repo, '--verify-tag', '--draft', '--generate-notes')
    gh('release', 'edit', tag, '--repo', repo, '--draft=false')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('command', choices=['build', 'publish', 'source', 'validate'])
    p.add_argument('--config', default='distribution.json')
    a = p.parse_args()
    c = read(a.config)
    for k in ('sourceName', 'developerName', 'description'):
        require(isinstance(c.get(k), str) and bool(c[k].strip()), f'Set {k} in {a.config}')
    if a.command == 'validate':
        print('Configuration valid; project discovery requires macOS.')
    elif a.command == 'build':
        build(c)
    else:
        repo = os.environ['GITHUB_REPOSITORY']
        require(bool(re.fullmatch(r'[\w.-]+/[\w.-]+', repo)), 'Invalid repository')
        if a.command == 'publish':
            publish(c, repo)
        else:
            write('site/source.json', assemble(c['sourceName'], manifests(repo)))
            Path('site/index.html').write_text('<!doctype html><meta charset="utf-8"><a href="source.json">AltStore Source</a>', encoding='utf-8')


if __name__ == '__main__':
    main()
