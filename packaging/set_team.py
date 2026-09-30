r"""set_team.py - stores the Thunderstore team (namespace) and optional dependency strings used by build_package.py.

The Thunderstore manifest has no team field (the team is chosen at upload); the team only names the local zip and the
R2Northstar\packages\<Team>-TF2_AI_Enhancements-<ver> install folder, and forms dependency strings Team-Name-x.y.z.

usage:
  python set_team.py <Team>                      set the team (A-Z a-z 0-9 _ only: Northstar's package folder regex)
  python set_team.py --website-url <url>         set manifest website_url ("" = none)
  python set_team.py --add-dep Team-Name-1.2.3   add a dependency (e.g. northstar-Northstar-1.31.9); --clear-deps
  python set_team.py --show
Writes tools\team.json (tmp + os.replace).
"""
import json, os, re, sys

P = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools', 'team.json')


def load():
    if os.path.exists(P):
        return json.loads(open(P, 'rb').read().decode('utf-8'))
    return {'team': 'TF2AI', 'website_url': '', 'dependencies': []}


def save(cfg):
    os.makedirs(os.path.dirname(P), exist_ok=True)
    with open(P + '.tmp', 'wb') as f:
        f.write(json.dumps(cfg, indent=2).encode('utf-8'))
    os.replace(P + '.tmp', P)


def main(argv):
    cfg = load()
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__); return 0
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--show':
            pass
        elif a == '--website-url':
            i += 1; url = argv[i]
            if url and not re.match(r'https?://', url): print('website_url must start with http:// or https://'); return 1
            cfg['website_url'] = url
        elif a == '--add-dep':
            i += 1; dep = argv[i]
            if not re.fullmatch(r'[A-Za-z0-9_]+-[A-Za-z0-9_]+-\d+\.\d+\.\d+', dep): print(f'bad dependency {dep!r}: Team-Name-x.y.z'); return 1
            if dep not in cfg['dependencies']: cfg['dependencies'].append(dep)
        elif a == '--clear-deps':
            cfg['dependencies'] = []
        elif a.startswith('-'):
            print(f'unknown option {a}'); return 1
        else:
            if not re.fullmatch(r'[A-Za-z0-9_]+', a): print(f'bad team {a!r}: only A-Z a-z 0-9 _'); return 1
            cfg['team'] = a
        i += 1
    save(cfg)
    print(json.dumps(cfg, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
