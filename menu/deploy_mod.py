"""Lint TF2NR.GraphicsToggles scripts, then copy the mod into R2Northstar/mods (game must be closed).
A UI compile error destroys the UI VM (the game sits at the menu), so deployment refuses on:
  * a line with an unterminated "string" (odd number of unescaped quotes outside // comments)
  * typed tables keyed by var ('table< var' is a compile error)
  * element methods (x.SetNavUp( ... ) etc.) in a typed script: "May not index into type var with type string"
    (run ai_menu1, 2026-09-29); only 'untyped' files may call methods on a var - use SetNavUpDown( array<var> )
"""
import os, re, sys, shutil, json
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'TF2NR.GraphicsToggles')
DST = 'C:/Program Files (x86)/Steam/steamapps/common/Titanfall2/R2Northstar/mods/TF2NR.GraphicsToggles'


def lint_nut(path):
    bad = []
    src = open(path, encoding='utf-8').read()
    typed = not re.match(r'(\s*//[^\n]*\n)*\s*untyped\b', src)
    for n, line in enumerate(src.split('\n'), 1):
        code = re.sub(r'\\.', '', line)           # drop escapes (\" \n): a backslash + the escaped char
        q = 0; i = 0
        while i < len(code):                       # count quotes up to a // comment outside strings
            if code[i] == '"': q ^= 1
            elif code.startswith('//', i) and not q: break
            i += 1
        if q: bad.append(f'{path}:{n}: unterminated string literal')
        if re.search(r'table\s*<\s*var\b', code[:i]): bad.append(f'{path}:{n}: table<var,...> is a compile error')
        if typed and re.search(r'\.\s*SetNav(Up|Down|Left|Right)\s*\(', code[:i]):
            bad.append(f'{path}:{n}: element method call on a var in a typed script is a compile error (use SetNavUpDown)')
    return bad


problems = []
for root, _, files in os.walk(SRC):
    for f in files:
        p = os.path.join(root, f)
        if f.endswith(('.nut', '.gnut')): problems += lint_nut(p)
        if f == 'mod.json':
            try: json.load(open(p, encoding='utf-8'))
            except Exception as e: problems.append(f'{p}: invalid JSON: {e}')
if problems:
    print('REFUSED:\n  ' + '\n  '.join(problems)); sys.exit(1)
if '--check' in sys.argv:
    print('lint ok'); sys.exit(0)
shutil.copytree(SRC, DST, dirs_exist_ok=True)
print('lint ok; deployed to', DST)
