r"""install_package.py - installs a package zip into a Northstar profile the way r2modman / Viper do: the zip is extracted
unchanged into <profile>\packages\<Team>-<name>-<version>\ (name/version from its manifest.json).

usage: python install_package.py <package.zip> --profile <profile dir> [--team TF2AI] [--replace]
Refuses to overwrite an existing package folder unless --replace (then only that folder is replaced).
"""
import argparse, json, os, re, shutil, sys, zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('zip')
    ap.add_argument('--profile', required=True)
    ap.add_argument('--team', default=None)
    ap.add_argument('--replace', action='store_true')
    a = ap.parse_args()
    z = zipfile.ZipFile(a.zip)
    m = json.loads(z.read('manifest.json').decode('utf-8-sig'))
    team = a.team
    if not team:
        base = os.path.basename(a.zip)
        mm = re.match(r'([A-Za-z0-9_]+)-' + re.escape(m['name']) + r'-', base)
        team = mm.group(1) if mm else 'TF2AI'
    folder = f"{team}-{m['name']}-{m['version_number']}"
    if not re.fullmatch(r'[a-zA-Z0-9_]+-[a-zA-Z0-9_]+-\d+\.\d+\.\d+', folder):
        sys.exit(f'{folder} does not match Northstar\'s package folder regex')
    dst = os.path.join(a.profile, 'packages', folder)
    if os.path.exists(dst):
        if not a.replace: sys.exit(f'{dst} exists (use --replace)')
        shutil.rmtree(dst)
    for n in z.namelist():
        if n.startswith('/') or '..' in n.split('/') or chr(92) in n: sys.exit(f'unsafe member {n}')
    os.makedirs(dst)
    z.extractall(dst)
    print(f'installed {a.zip} -> {dst} ({len(z.namelist())} files)')


if __name__ == '__main__':
    main()
