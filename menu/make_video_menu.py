# Builds TF2NR.GraphicsToggles/mod/resource/ui/menus/video.menu from the CRC-verified vanilla copy.
# Vanilla source: vanilla/video.menu (extracted from englishclient_frontend.bsp.pak000_dir.vpk) (15187 bytes, CRC32 829977867 == VPK index entry).
#
# v1.0 layout (owner decision 2026-09-29): the vanilla Video page is left exactly as shipped, plus ONE clickable row,
# "AI Enhancements" (BtnAIEnhancements), which opens the TF2NR_AIMenu page (resource/ui/menus/tf2nr_ai.menu, built by
# make_ai_menu.py). Everything the v0.3-v0.5 layouts did (DLSS 5 NR / DLSS 5 Adjustments / DLSS SR rows in the left
# column, Ambient Occlusion moved to the right column, Frame Generation (FSR) as row 10 of the right column, their nav
# links) is gone: every vanilla row is back in its vanilla place with its vanilla nav.
#
# Placement: the vanilla left ADVANCED column has 7 rows (V-Sync .. Ambient Occlusion: 621 + 7*40 = 901 @1080); the entry
# is its 8th row, under Ambient Occlusion, ending at 621 + 8*40 = 941 < footer top (r90 = 990). The right column keeps its
# 8 vanilla rows (941). Nav: Ambient Occlusion -> AI Enhancements -> Sun Shadow Detail (top of the right column), i.e.
# the entry is spliced into the vanilla linear nav chain (vanilla: Ambient Occlusion <-> Sun Shadow Detail).
import zlib, os, re
S = os.path.dirname(os.path.abspath(__file__)) + '/'
src = S + 'vanilla/video.menu'
dst = S + 'TF2NR.GraphicsToggles/mod/resource/ui/menus/video.menu'
b = open(src, 'rb').read()
assert len(b) == 15187 and zlib.crc32(b) == 829977867, 'vanilla video.menu mismatch'
vanilla = b.decode('ascii')
t = vanilla
N = '\r\n'


def block(text, name):
    """Return (start, end) of the element block `name { ... }` at depth 2 (element names are unique)."""
    m = re.search(r'\r\n\t\t' + re.escape(name) + r'[^\r\n]*\r\n\t\t\{\r\n', text)
    assert m, name
    end = text.index('\r\n\t\t}\r\n', m.end()) + len('\r\n\t\t}')
    return m.start() + 2, end


def edit_block(text, name, pairs):
    s, e = block(text, name)
    body = text[s:e]
    for old, new in pairs:
        assert body.count(old) == 1, (name, old, body.count(old))
        body = body.replace(old, new)
    return text[:s] + body + text[e:]


# 1) splice the entry into the vanilla nav chain: Ambient Occlusion <-> AI Enhancements <-> Sun Shadow Detail
t = edit_block(t, 'SwchAmbientOcclusion', [('navDown\t\t\t\t\tSwchSunShadowDetail', 'navDown\t\t\t\t\tBtnAIEnhancements')])
t = edit_block(t, 'SwchSunShadowDetail', [('navUp\t\t\t\t\tSwchAmbientOcclusion', 'navUp\t\t\t\t\tBtnAIEnhancements')])

# 2) the entry, right after the SwchAmbientOcclusion block
entry = (
"\t\t// ---- TF2NR AI Enhancements v1.0: row 8 of the left ADVANCED column (621 + 8*40 = 941 < footer top 990 @1080) ----" + N +
"\t\t// Plain clickable row (same element type as vanilla BtnGamepadCustomResetToDefaults in controls_advanced_look.menu):" + N +
"\t\t// no list, no ConVar, no AdvancedVideoButtonClass (it must not arm the Apply footer); the script opens TF2NR_AIMenu" + N +
"\t\t// on UIE_CLICK, the same wiring as vanilla BtnAdvancedHud -> AdvancedHudMenu." + N +
"\t\tBtnAIEnhancements" + N + "\t\t{" + N +
"\t\t\tControlName\t\t\t\tRuiButton" + N + "\t\t\tInheritProperties\t\tSwitchButton" + N + "\t\t\tstyle\t\t\t\t\tDialogListButton" + N +
"\t\t\tpin_to_sibling\t\t\tSwchAmbientOcclusion" + N + "\t\t\tpin_corner_to_sibling\tTOP_LEFT" + N + "\t\t\tpin_to_sibling_corner\tBOTTOM_LEFT" + N +
"\t\t\tnavUp\t\t\t\t\tSwchAmbientOcclusion" + N + "\t\t\tnavDown\t\t\t\t\tSwchSunShadowDetail" + N +
"\t\t}")
s, e = block(t, 'SwchAmbientOcclusion')
t = t[:e] + N + entry + t[e:]

out = t.encode('ascii')
tmp = dst + '.tmp'
open(tmp, 'wb').write(out)
os.replace(tmp, dst)
print('wrote', dst, len(out), 'bytes, crc32', zlib.crc32(out))

# 3) sanity: brace balance + unique element names at depth 2 + every nav/pin target exists + column geometry
depth = 0; names = []; last_tok = None
for line in t.split('\r\n'):
    s_ = line.split('//')[0].strip()
    if s_ == '{':
        depth += 1
        if depth == 3 and last_tok: names.append(last_tok)
        continue
    if s_ == '}':
        depth -= 1; continue
    if s_: last_tok = s_.split()[0]
assert depth == 0, depth
dups = {n for n in names if names.count(n) > 1}
refs = set(re.findall(r'(?:navUp|navDown|pin_to_sibling)\s+(\w+)', t))
print('elements', len(names), 'dups', dups, 'dangling refs', sorted(refs - set(names)))
assert not dups and not (refs - set(names))


def field(name, key, text=None):
    text = t if text is None else text
    if not re.search(r'\r\n\t\t' + re.escape(name) + r'[^\r\n]*\r\n\t\t\{', text):
        return None   # space-indented vanilla blocks (Vignette): not a row
    s_, e_ = block(text, name)
    m = re.search(r'\n\t\t\t' + key + r'\s+(\S+)', text[s_:e_])
    return m.group(1) if m else None


# walk each column from its head via pin_to_sibling and check nav symmetry
children = {}
for n in names:
    p = field(n, 'pin_to_sibling')
    if p: children.setdefault(p, []).append(n)
for head in ('SwchVSync', 'SwchSunShadowDetail'):
    col = [head]
    while True:
        nxt = [c for c in children.get(col[-1], []) if field(c, 'pin_to_sibling_corner') == 'BOTTOM_LEFT' and c.startswith(('Swch', 'Sld', 'Btn'))]
        if not nxt: break
        col.append(nxt[0])
    print(head, 'column', len(col), 'rows ->', 621 + len(col) * 40, '(footer 990):', col)
    assert 621 + len(col) * 40 <= 990
asym = 0
for n in names:
    d = field(n, 'navDown')
    if d and field(d, 'navUp') and field(d, 'navUp') != n:
        asym += 1
        print('nav asymmetry:', n, '->', d, 'but', d, 'navUp =', field(d, 'navUp'))
assert asym == 0, asym

# 4) the only differences from vanilla: the two nav edits + the inserted entry block
assert t.count(N + entry) == 1
stripped = t.replace(N + entry, '', 1)
stripped = stripped.replace('navDown\t\t\t\t\tBtnAIEnhancements', 'navDown\t\t\t\t\tSwchSunShadowDetail', 1)
stripped = stripped.replace('navUp\t\t\t\t\tBtnAIEnhancements', 'navUp\t\t\t\t\tSwchAmbientOcclusion', 1)
assert stripped == vanilla, 'video.menu differs from vanilla beyond the AI Enhancements entry'
print('vanilla check: identical to vanilla except the BtnAIEnhancements block and 2 nav links')
