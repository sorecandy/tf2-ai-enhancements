# Builds TF2NR.GraphicsToggles/mod/resource/ui/menus/tf2nr_dlss5.menu (v0.3 layout; 1.0.0 review fixes: title
# "DLSS 5 ADJUSTMENTS" = the row that opens it, subheaders at ypos 0 instead of vanilla's -16 so NEURAL MODEL / LOOK no
# longer cover the bottom of the row above - a pinned ypos is ADDED, so -16 pulled the 45-tall bar 16 units up).
# The "DLSS 5 Adjustments" submenu, opened from the BtnDLSS5Tuning row of the AI Enhancements page (tf2nr_ai.menu,
# make_ai_menu.py); up to 0.5.1 it was opened from a Video-menu row of the same name.
# Every element type/property below is copied from a vanilla TF2 menu (extracted copies in research/vanilla_ui):
#   header block .......... audio.menu / controls_advanced_look.menu (Vignette, MenuTitle, ImgTopBar, MatchmakingStatus)
#   description label ...... audio.menu LblMenuItemDescription (OptionMenuTooltip, xpos 975, ypos 168)
#   DialogListButton+ConVar  audio.menu SwchSubtitles
#   plain clickable row .... controls_advanced_look.menu BtnGamepadCustomResetToDefaults (SwitchButton, no list)
#   SliderControl+conCommand audio.menu SldMasterVolume / controls_advanced_look.menu SldGamepadCustomHipYaw
#   subheader .............. controls_advanced_look.menu ImgGeneralSubheaderBackground3 (xpos 22, ypos -16 -> 0 here, next row xpos -22)
#                            + video.menu LblAdvVideoSubheaderText (SubheaderText pinned LEFT/LEFT)
# Ranges here are the UI ranges; the add-on clamps again (Mod Settings text fields accept any number).
import os, re
S = os.path.dirname(os.path.abspath(__file__)) + '/'
dst = S + 'TF2NR.GraphicsToggles/mod/resource/ui/menus/tf2nr_dlss5.menu'
N = '\r\n'

# (element, kind, convar, min, max, step | list)
ROWS = [
    ('SwchDLSS5NR',        'list',   'tf2nr_dlss5_nr', [('#SETTING_OFF', 0), ('#SETTING_ON', 1)]),
    ('BtnDLSS5Reset',      'button', None, None),
    ('ImgSubheaderModel',  'header', 'NEURAL MODEL', None),
    ('SldNRIntensity',     'slider', 'tf2nr_nr_intensity',      (0.0, 2.0, 0.1)),
    ('SldNRLocalTone',     'slider', 'tf2nr_nr_local_tone',     (0.0, 2.0, 0.1)),
    ('SldNRStructure',     'slider', 'tf2nr_nr_structure',      (0.0, 2.0, 0.1)),
    ('SldNRSkin',          'slider', 'tf2nr_nr_skin',           (0.0, 1.0, 0.05)),
    ('SwchNRStyle',        'list',   'tf2nr_nr_style', [('Default', 0), ('Natural', 1), ('Cinematic', 2)]),
    ('ImgSubheaderLook',   'header', 'LOOK (RESHAPES THE RESULT)', None),
    ('SldNRLookStrength',  'slider', 'tf2nr_nr_look_strength',  (0.0, 2.0, 0.1)),
    ('SldNRLookColour',    'slider', 'tf2nr_nr_look_colour',    (0.0, 2.0, 0.1)),
    ('SldNRLookHue',       'slider', 'tf2nr_nr_look_hue',       (0.0, 2.0, 0.1)),
    ('SldNRLookDarken',    'slider', 'tf2nr_nr_look_darken',    (0.0, 2.0, 0.1)),
    ('SldNRLookDetail',    'slider', 'tf2nr_nr_look_detail',    (0.0, 2.0, 0.1)),
    ('SwchNRStabilize',    'list',   'tf2nr_nr_stabilize', [('#SETTING_OFF', 0), ('Static', 1), ('Motion', 2)]),
]
focusable = [r[0] for r in ROWS if r[1] != 'header']


def kv(k, v, ind=3):
    # vanilla alignment: tab stop 4, value column at 28 chars from the key start
    tabs = max(1, (24 - len(k) + 3) // 4)
    return '\t' * ind + k + '\t' * tabs + v + N


def fmt(x):
    return ('%.3f' % x).rstrip('0').rstrip('.') if x != int(x) else '%.1f' % x


out = []
w = out.append
w('resource/ui/menus/tf2nr_dlss5.menu' + N + '{' + N + '\tmenu' + N + '\t{' + N)
for k, v in [('ControlName', 'Frame'), ('xpos', '0'), ('ypos', '0'), ('zpos', '3'), ('wide', 'f0'), ('tall', 'f0'),
             ('autoResize', '0'), ('visible', '1'), ('enabled', '1'), ('pinCorner', '0'), ('PaintBackgroundType', '0'),
             ('infocus_bgcolor_override', '"0 0 0 0"'), ('outoffocus_bgcolor_override', '"0 0 0 0"')]:
    w(kv(k, v, 2))
w(N)
w('\t\tVignette' + N + '\t\t{' + N + kv('ControlName', 'ImagePanel') + kv('InheritProperties', 'MenuVignette') + '\t\t}' + N + N)
w('\t\tMenuTitle' + N + '\t\t{' + N + kv('ControlName', 'Label') + kv('InheritProperties', 'MenuTitle') +
  kv('labelText', '"DLSS 5 ADJUSTMENTS"') + '\t\t}' + N + N)
w('\t\tImgTopBar' + N + '\t\t{' + N + kv('ControlName', 'ImagePanel') + kv('InheritProperties', 'MenuTopBar') + '\t\t}' + N + N)
w('\t\tMatchmakingStatus' + N + '\t\t{' + N + kv('ControlName', 'CNestedPanel') + kv('xpos', '0') + kv('ypos', '0') +
  kv('wide', 'f0') + kv('tall', 'f0') + kv('visible', '1') +
  kv('controlSettingsFile', '"resource/ui/menus/panels/matchmaking_status.res"') + '\t\t}' + N + N)
w('\t\tLblMenuItemDescription' + N + '\t\t{' + N + kv('ControlName', 'Label') + kv('InheritProperties', 'OptionMenuTooltip') +
  kv('classname', 'MenuItemDescriptionClass') + kv('xpos', '975') + kv('ypos', '168') + '\t\t}' + N + N)
w('\t\t// ---- rows: 160 + 2*40 + 2*45 + 10*40 = 730 < footer 990 (subheader bars ypos 0) -----------------------------------' + N)

prev = None
for i, (name, kind, cv, extra) in enumerate(ROWS):
    fi = focusable.index(name) if name in focusable else -1
    up = focusable[fi - 1] if fi >= 0 else None
    down = focusable[(fi + 1) % len(focusable)] if fi >= 0 else None
    b = '\t\t' + name + N + '\t\t{' + N
    if kind == 'header':
        b += kv('ControlName', 'ImagePanel') + kv('InheritProperties', 'SubheaderBackground')
        b += kv('xpos', '22') + kv('ypos', '0')
        b += kv('pin_to_sibling', prev) + kv('pin_corner_to_sibling', 'TOP_LEFT') + kv('pin_to_sibling_corner', 'BOTTOM_LEFT')
        b += '\t\t}' + N
        lbl = 'Lbl' + name[3:]
        b += '\t\t' + lbl + N + '\t\t{' + N + kv('ControlName', 'Label') + kv('InheritProperties', 'SubheaderText')
        b += kv('pin_to_sibling', name) + kv('pin_corner_to_sibling', 'LEFT') + kv('pin_to_sibling_corner', 'LEFT')
        b += kv('labelText', '"' + cv + '"') + '\t\t}' + N
        w(b); prev = name; continue
    if kind == 'slider':
        b += kv('ControlName', 'SliderControl') + kv('InheritProperties', 'SliderControl')
    else:
        b += kv('ControlName', 'RuiButton') + kv('InheritProperties', 'SwitchButton') + kv('style', 'DialogListButton')
    if prev is None:
        b += kv('xpos', '120') + kv('ypos', '160') + kv('tabPosition', '1')
    else:
        if prev.startswith('ImgSubheader'):
            b += kv('xpos', '-22')
        b += kv('pin_to_sibling', prev) + kv('pin_corner_to_sibling', 'TOP_LEFT') + kv('pin_to_sibling_corner', 'BOTTOM_LEFT')
    b += kv('navUp', up) + kv('navDown', down)
    if kind == 'list':
        b += kv('ConVar', '"' + cv + '"') + '\t\t\tlist' + N + '\t\t\t{' + N
        for label, val in extra:
            b += '\t\t\t\t"' + label + '"\t\t' + str(val) + N
        b += '\t\t\t}' + N
    elif kind == 'slider':
        lo, hi, st = extra
        b += kv('conCommand', '"' + cv + '"') + kv('minValue', fmt(lo)) + kv('maxValue', fmt(hi)) + kv('stepSize', fmt(st))
        b += kv('inverseFill', '0')
        b += '\t\t\tBtnDropButton' + N + '\t\t\t{' + N
        for k, v in [('ControlName', 'RuiButton'), ('InheritProperties', 'WideButton'), ('clip', '0'), ('autoResize', '1'),
                     ('pinCorner', '0'), ('visible', '1'), ('enabled', '1'), ('style', 'SliderButton'), ('command', '""')]:
            b += kv(k, v, 4)
        b += '\t\t\t}' + N
    b += '\t\t}' + N
    w(b); prev = name

w(N + '\t\tFooterButtons' + N + '\t\t{' + N + kv('ControlName', 'CNestedPanel') + kv('InheritProperties', 'FooterButtons') + '\t\t}' + N)
w('\t}' + N + '}' + N)
t = ''.join(out)
data = t.encode('ascii')
tmp = dst + '.tmp'
open(tmp, 'wb').write(data)
os.replace(tmp, dst)
print('wrote', dst, len(data), 'bytes')

# sanity: braces, unique names, refs, quotes per line
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
