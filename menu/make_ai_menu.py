# Builds TF2NR.GraphicsToggles/mod/resource/ui/menus/tf2nr_ai.menu (v1.0): the "AI Enhancements" page, opened by the
# single BtnAIEnhancements row of the Video page (make_video_menu.py). Script: mod/scripts/vscripts/ui/tf2nr_video_toggles.nut
# (menu name TF2NR_AIMenu).
# Every element type/property is copied from vanilla / Northstar menus (same sources as make_dlss5_menu.py):
#   header block .......... audio.menu / controls_advanced_look.menu (Vignette, MenuTitle, ImgTopBar, MatchmakingStatus)
#   description label ...... audio.menu LblMenuItemDescription (OptionMenuTooltip, xpos 975, ypos 168; scheme tall 562)
#   DialogListButton+ConVar  audio.menu SwchSubtitles (static list block)
#   script-filled list ..... Northstar custom_match_settings.menu BtnSettingN (RuiButton, style DialogListButton, no list
#                            block): the script adds the items with Hud_DialogList_AddListItem once at init (lists cannot
#                            be cleared) and reads/sets them with Hud_Get/SetDialogListSelectionValue
#   plain clickable row .... controls_advanced_look.menu BtnGamepadCustomResetToDefaults (SwitchButton, no list)
#
# Rows (SwitchButton = 740x40): 160 + 5*40 = 360 < footer 990 (@1080).
#   Upscaler ............. script list Off / DLSS / FSR 3 (tf2nr_dlss_sr 0 | tf2nr_upscaler 1 | 2, with tf2nr_upscale_last)
#   Quality .............. two script lists at the SAME spot, one visible at a time: SwchQualityDLSS (DLAA, Quality, ...)
#                          and SwchQualityFSR (Native AA, Quality, ...); the script swaps them and re-links the nav
#   DLSS 5 ............... ConVar list tf2nr_dlss5_nr Off / On
#   DLSS 5 Adjustments ... plain row, opens TF2NR_DLSS5Menu (tf2nr_dlss5.menu)
#   Frame Generation ..... ConVar list tf2nr_fsr_fg Off / On
import os, re
S = os.path.dirname(os.path.abspath(__file__)) + '/'
dst = S + 'TF2NR.GraphicsToggles/mod/resource/ui/menus/tf2nr_ai.menu'
N = '\r\n'

ON_OFF = [('#SETTING_OFF', 0), ('#SETTING_ON', 1)]
# (element, kind, convar | None, list | None, pinned to (None = previous row), extra properties)
ROWS = [
    ('SwchUpscaler',    'scriptlist', None,             None,   None,           []),
    ('SwchQualityDLSS', 'scriptlist', None,             None,   None,           []),
    ('SwchQualityFSR',  'scriptlist', None,             None,   'SwchUpscaler', [('visible', '0'), ('enabled', '0')]),
    # pinned to the Upscaler row, 40 below its bottom, so its place never depends on which Quality row is hidden.
    # Here a pinned ypos is ADDED (run ai_menu2 2026-09-29: ypos -40 drew this row on top of the Upscaler row).
    ('SwchDLSS5NR',     'list',       'tf2nr_dlss5_nr', ON_OFF, 'SwchUpscaler', [('ypos', '40')]),
    ('BtnDLSS5Tuning',  'button',     None,             None,   None,           []),
    ('SwchFSRFG',       'list',       'tf2nr_fsr_fg',   ON_OFF, None,           []),
]
# static nav (the script re-links Upscaler.navDown / DLSS5NR.navUp to whichever Quality row is visible)
NAV = {
    'SwchUpscaler':    ('SwchFSRFG', 'SwchQualityDLSS'),
    'SwchQualityDLSS': ('SwchUpscaler', 'SwchDLSS5NR'),
    'SwchQualityFSR':  ('SwchUpscaler', 'SwchDLSS5NR'),
    'SwchDLSS5NR':     ('SwchQualityDLSS', 'BtnDLSS5Tuning'),
    'BtnDLSS5Tuning':  ('SwchDLSS5NR', 'SwchFSRFG'),
    'SwchFSRFG':       ('BtnDLSS5Tuning', 'SwchUpscaler'),
}


def kv(k, v, ind=3):
    # vanilla alignment: tab stop 4, value column at 28 chars from the key start
    tabs = max(1, (24 - len(k) + 3) // 4)
    return '\t' * ind + k + '\t' * tabs + v + N


out = []
w = out.append
w('resource/ui/menus/tf2nr_ai.menu' + N + '{' + N + '\tmenu' + N + '\t{' + N)
for k, v in [('ControlName', 'Frame'), ('xpos', '0'), ('ypos', '0'), ('zpos', '3'), ('wide', 'f0'), ('tall', 'f0'),
             ('autoResize', '0'), ('visible', '1'), ('enabled', '1'), ('pinCorner', '0'), ('PaintBackgroundType', '0'),
             ('infocus_bgcolor_override', '"0 0 0 0"'), ('outoffocus_bgcolor_override', '"0 0 0 0"')]:
    w(kv(k, v, 2))
w(N)
w('\t\tVignette' + N + '\t\t{' + N + kv('ControlName', 'ImagePanel') + kv('InheritProperties', 'MenuVignette') + '\t\t}' + N + N)
w('\t\tMenuTitle' + N + '\t\t{' + N + kv('ControlName', 'Label') + kv('InheritProperties', 'MenuTitle') +
  kv('labelText', '"AI ENHANCEMENTS"') + '\t\t}' + N + N)
w('\t\tImgTopBar' + N + '\t\t{' + N + kv('ControlName', 'ImagePanel') + kv('InheritProperties', 'MenuTopBar') + '\t\t}' + N + N)
w('\t\tMatchmakingStatus' + N + '\t\t{' + N + kv('ControlName', 'CNestedPanel') + kv('xpos', '0') + kv('ypos', '0') +
  kv('wide', 'f0') + kv('tall', 'f0') + kv('visible', '1') +
  kv('controlSettingsFile', '"resource/ui/menus/panels/matchmaking_status.res"') + '\t\t}' + N + N)
w('\t\tLblMenuItemDescription' + N + '\t\t{' + N + kv('ControlName', 'Label') + kv('InheritProperties', 'OptionMenuTooltip') +
  kv('classname', 'MenuItemDescriptionClass') + kv('xpos', '975') + kv('ypos', '168') + '\t\t}' + N + N)
w('\t\t// ---- rows: 160 + 5*40 = 360 < footer 990; SwchQualityFSR sits on SwchQualityDLSS (one visible at a time) ------' + N)

prev = None
for name, kind, cv, items, pin, extra in ROWS:
    up, down = NAV[name]
    b = '\t\t' + name + N + '\t\t{' + N
    b += kv('ControlName', 'RuiButton') + kv('InheritProperties', 'SwitchButton') + kv('style', 'DialogListButton')
    if prev is None:
        b += kv('xpos', '120') + kv('ypos', '160') + kv('tabPosition', '1')
    else:
        b += kv('pin_to_sibling', pin or prev) + kv('pin_corner_to_sibling', 'TOP_LEFT') + kv('pin_to_sibling_corner', 'BOTTOM_LEFT')
    for k, v in extra:
        b += kv(k, v)
    b += kv('navUp', up) + kv('navDown', down)
    if kind == 'list':
        b += kv('ConVar', '"' + cv + '"') + '\t\t\tlist' + N + '\t\t\t{' + N
        for label, val in items:
            b += '\t\t\t\t"' + label + '"\t\t' + str(val) + N
        b += '\t\t\t}' + N
    elif kind == 'scriptlist':
        b += '\t\t\t// list items are added by the script (Hud_DialogList_AddListItem)' + N
    b += '\t\t}' + N
    w(b)
    if pin is None or kind != 'scriptlist':
        prev = name   # SwchQualityFSR (pinned onto the DLSS row's spot) is not a layout step

w(N + '\t\tFooterButtons' + N + '\t\t{' + N + kv('ControlName', 'CNestedPanel') + kv('InheritProperties', 'FooterButtons') + '\t\t}' + N)
w('\t}' + N + '}' + N)
t = ''.join(out)
data = t.encode('ascii')
tmp = dst + '.tmp'
open(tmp, 'wb').write(data)
os.replace(tmp, dst)
print('wrote', dst, len(data), 'bytes')

# sanity: braces, unique names, refs, quotes per line, nav symmetry (except the swapped Quality pair), geometry
depth = 0; names = []; last = None
for ln in t.split(N):
    s_ = ln.split('//')[0].strip()
    assert s_.count('"') % 2 == 0, ln
    if s_ == '{':
        depth += 1
        if depth == 3 and last: names.append(last)
        continue
    if s_ == '}':
        depth -= 1; continue
    if s_: last = s_.split()[0]
assert depth == 0, depth
dups = {n for n in names if names.count(n) > 1}
refs = set(re.findall(r'(?:navUp|navDown|pin_to_sibling)\s+(\w+)', t))
print('elements', len(names), 'dups', dups, 'dangling', sorted(refs - set(names)))
assert not dups and not (refs - set(names))
for n, (up, down) in NAV.items():
    if n == 'SwchQualityFSR':
        continue
    assert NAV[down][0] == n, (n, down)
rows_in_layout = [r[0] for r in ROWS if not (r[1] == 'scriptlist' and r[4])]
print('layout rows', len(rows_in_layout), '->', 160 + 40 * len(rows_in_layout), '(footer 990):', rows_in_layout)
assert 160 + 40 * len(rows_in_layout) <= 990
