r"""validate_package.py - checks a TF2_AI_Enhancements package (zip or unpacked folder) against Thunderstore's package
format and what Northstar 1.31.x / r2modman / Viper need.

usage: python validate_package.py <package.zip | package_dir>        exit 0 = OK (warnings allowed), 1 = errors

Thunderstore (thunderstore.io package format docs):
  - zip root holds manifest.json, icon.png, README.md (no wrapping folder)
  - manifest.json: UTF-8 JSON (no BOM) with name [A-Za-z0-9_]+, version_number Major.Minor.Patch, website_url (string,
    empty or http(s) URL), description <= 250 chars, dependencies = list of "Team-Name-x.y.z"
  - icon.png: PNG, exactly 256x256
Northstar 1.31.x (NorthstarLauncher primedev/mods/modmanager.cpp, plugins/pluginmanager.cpp, plugins.cpp):
  - the package is installed unchanged to <profile>\packages\<Team>-<Name>-<x.y.z>\ (folder regex
    ([a-zA-Z0-9_]+)-([a-zA-Z0-9_]+)-(\d+\.\d+\.\d+)); mods load from mods\<mod>\mod.json, plugins from plugins\*.dll
    (non-recursive; plugins\lib is added to the DLL search path, never loaded as plugins)
  - a plugin must export CreateInterface (x64 PE)
Package policy (this mod):
  - mod.json valid JSON with Name + Version == manifest version_number; .nut files pass the UI-compile lint
  - plugins\lib holds ReShade64.dll, ReShade.ini, tf2-dlss-sr / tf2-nr-bridge / tf2-fg .addon64, nvngx_dlss.dll
  - nothing that must not ship: renodx-dlss5, nvngx_dlssnr.dll, DLSS5-Feeder, logs, csv, screenshots, dumps, probe /
    dev files, backups, build intermediates
"""
import io
import json
import os
import re
import struct
import sys
import zipfile

REQUIRED_LIB = ['plugins/lib/ReShade64.dll', 'plugins/lib/proxy/dxgi.dll', 'plugins/lib/ReShade.ini', 'plugins/lib/tf2-dlss-sr.addon64',
                'plugins/lib/tf2-nr-bridge.addon64', 'plugins/lib/tf2-fg.addon64', 'plugins/lib/nvngx_dlss.dll']
DENY_NAME = [r'(^|/)renodx-dlss5\.addon64$', r'(^|/)nvngx_dlssnr\.dll$', r'(^|/)dlss5-feed', r'(^|/)DLSS5_Feed\.fx$',
             r'\.log$', r'\.csv$', r'\.dmp$', r'\.pdb$', r'\.obj$', r'\.exp$', r'\.ilk$', r'(^|/)[^/]+\.lib$', r'\.bak$',
             r'\.tmp$', r'(^|/)tf2probe(/|$)', r'(^|/)tf2-frame-probe\.cfg$', r'(^|/)Verify-[^/]*\.ps1$', r'(^|/)backups?/',
             r'(^|/)DLSS5 Screenshots/', r'\.(jpg|jpeg|bmp|tga|dds)$', r'(^|/)reshade-shaders/', r'(^|/)ReShadePreset\.ini$',
             r'(^|/)(profile\.cfg|videoconfig\.txt)$', r'\.bin$', r'\.f32$']


class Pkg:
    def __init__(self, path):
        self.path = path
        self.zip = None
        if os.path.isdir(path):
            self.names = []
            for root, _, files in os.walk(path):
                for f in files:
                    self.names.append(os.path.relpath(os.path.join(root, f), path).replace('\\', '/'))
        else:
            self.zip = zipfile.ZipFile(path)
            self.names = [n for n in self.zip.namelist() if not n.endswith('/')]
        self.names.sort()

    def read(self, name):
        if self.zip:
            return self.zip.read(name)
        with open(os.path.join(self.path, *name.split('/')), 'rb') as f:
            return f.read()


def pe_info(data):
    """(machine, [export names]) of a PE image, all reads bounds-checked; raises ValueError on a malformed file."""
    def u16(o):
        if o < 0 or o + 2 > len(data): raise ValueError('out of range')
        return struct.unpack_from('<H', data, o)[0]

    def u32(o):
        if o < 0 or o + 4 > len(data): raise ValueError('out of range')
        return struct.unpack_from('<I', data, o)[0]
    if data[:2] != b'MZ': raise ValueError('no MZ header')
    pe = u32(0x3C)
    if data[pe:pe + 4] != b'PE\0\0': raise ValueError('no PE header')
    machine = u16(pe + 4)
    nsec = u16(pe + 6)
    opt_size = u16(pe + 20)
    opt = pe + 24
    magic = u16(opt)
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva = u32(dd)
    secs = []
    so = opt + opt_size
    for i in range(nsec):
        b = so + 40 * i
        secs.append((u32(b + 12), max(u32(b + 8), u32(b + 16)), u32(b + 20)))   # rva, size, raw ptr

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz: return raw + (rva - va)
        raise ValueError('rva not in a section')
    names = []
    if exp_rva:
        e = off(exp_rva)
        n = u32(e + 24)
        np_ = off(u32(e + 32))
        for i in range(min(n, 4096)):
            no = off(u32(np_ + 4 * i))
            end = data.find(b'\0', no, no + 512)
            if end < 0: raise ValueError('bad export name')
            names.append(data[no:end].decode('ascii', 'replace'))
    return machine, names


def lint_nut(name, text):
    bad = []
    for n, line in enumerate(text.split('\n'), 1):
        code = re.sub(r'\\.', '', line)
        q = 0; i = 0
        while i < len(code):
            if code[i] == '"': q ^= 1
            elif code.startswith('//', i) and not q: break
            i += 1
        if q: bad.append(f'{name}:{n}: unterminated string literal (UI compile error)')
        if re.search(r'table\s*<\s*var\b', code[:i]): bad.append(f'{name}:{n}: table<var,...> (UI compile error)')
    return bad


def validate(path, quiet=False):
    errors, warns, notes = [], [], []
    try:
        pkg = Pkg(path)
    except Exception as e:
        print(f'INVALID: cannot open {path}: {e}')
        return 1
    names = pkg.names
    nameset = set(names)
    if pkg.zip:
        for zi in pkg.zip.infolist():
            n = zi.filename
            if n.startswith('/') or '\\' in n or re.match(r'^[A-Za-z]:', n) or '..' in n.split('/'):
                errors.append(f'unsafe zip path {n!r}')
        bad = pkg.zip.testzip()
        if bad: errors.append(f'corrupt zip member {bad}')
    for req in ('manifest.json', 'icon.png', 'README.md'):
        if req not in nameset:
            errors.append(f'{req} missing at the package root')

    manifest = {}
    if 'manifest.json' in nameset:
        raw = pkg.read('manifest.json')
        if raw.startswith(b'\xef\xbb\xbf'): errors.append('manifest.json has a UTF-8 BOM')
        try:
            manifest = json.loads(raw.decode('utf-8-sig'))
        except Exception as e:
            errors.append(f'manifest.json is not valid UTF-8 JSON: {e}')
        if manifest:
            for k in ('name', 'version_number', 'website_url', 'description', 'dependencies'):
                if k not in manifest: errors.append(f'manifest.json lacks "{k}"')
            n = manifest.get('name', '')
            if not isinstance(n, str) or not re.fullmatch(r'[A-Za-z0-9_]{1,128}', n): errors.append(f'manifest name {n!r} invalid')
            v = manifest.get('version_number', '')
            if not isinstance(v, str) or not re.fullmatch(r'\d+\.\d+\.\d+', v): errors.append(f'version_number {v!r} invalid')
            w = manifest.get('website_url', '')
            if not isinstance(w, str) or len(w) > 1024 or (w and not re.match(r'https?://', w)): errors.append(f'website_url {w!r} invalid')
            if not w: warns.append('website_url is empty (allowed; Thunderstore shows no website link)')
            d = manifest.get('description', '')
            if not isinstance(d, str) or len(d) > 250: errors.append(f'description must be a string of <= 250 chars (has {len(d) if isinstance(d, str) else "?"})')
            deps = manifest.get('dependencies', [])
            if not isinstance(deps, list): errors.append('dependencies must be a list')
            else:
                for dep in deps:
                    if not isinstance(dep, str) or not re.fullmatch(r'[A-Za-z0-9_]+-[A-Za-z0-9_]+-\d+\.\d+\.\d+', dep):
                        errors.append(f'dependency {dep!r} is not Team-Name-x.y.z')
            extra = set(manifest) - {'name', 'version_number', 'website_url', 'description', 'dependencies', 'installers'}
            if extra: warns.append(f'manifest has extra keys {sorted(extra)}')

    if 'icon.png' in nameset:
        b = pkg.read('icon.png')
        if b[:8] != b'\x89PNG\r\n\x1a\n' or b[12:16] != b'IHDR': errors.append('icon.png is not a PNG')
        else:
            w, h = struct.unpack('>II', b[16:24])
            if (w, h) != (256, 256): errors.append(f'icon.png is {w}x{h}, must be 256x256')
    if 'README.md' in nameset:
        try:
            if not pkg.read('README.md').decode('utf-8').strip(): errors.append('README.md is empty')
        except UnicodeDecodeError:
            errors.append('README.md is not UTF-8')

    # layout: only known top-level entries
    for n in names:
        top = n.split('/')[0]
        if top not in ('manifest.json', 'icon.png', 'README.md', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.txt', 'licenses', 'mods', 'plugins'):
            warns.append(f'unexpected top-level entry {n}')
        for pat in DENY_NAME:
            if re.search(pat, n, re.I):
                errors.append(f'must not ship: {n} (rule {pat})')
                break

    # mods
    mod_dirs = sorted({n.split('/')[1] for n in names if n.startswith('mods/') and n.count('/') >= 2})
    if not mod_dirs: errors.append('no mods/<mod>/ folder')
    for m in mod_dirs:
        mj = f'mods/{m}/mod.json'
        if mj not in nameset:
            errors.append(f'{mj} missing'); continue
        try:
            mod = json.loads(pkg.read(mj).decode('utf-8-sig'))
        except Exception as e:
            errors.append(f'{mj} invalid JSON: {e}'); continue
        for k in ('Name', 'Version'):
            if k not in mod: errors.append(f'{mj} lacks "{k}"')
        if manifest and mod.get('Version') != manifest.get('version_number'):
            errors.append(f'{mj} Version {mod.get("Version")!r} != manifest version_number {manifest.get("version_number")!r}')
        for sc in mod.get('Scripts', []):
            p = f'mods/{m}/mod/scripts/vscripts/{sc.get("Path", "")}'
            if p not in nameset: errors.append(f'{mj} script {sc.get("Path")} not found at {p}')
        notes.append(f'mod {mod.get("Name")} {mod.get("Version")} ({sum(1 for n in names if n.startswith(f"mods/{m}/"))} files)')
        if mod.get('Name') == 'TF2NR.GraphicsToggles':   # policy: a fresh install starts with every feature off
            defaults = {c.get('Name'): c.get('DefaultValue') for c in mod.get('ConVars', []) if isinstance(c, dict)}
            for cv, want in (('tf2nr_dlss5_nr', '0'), ('tf2nr_dlss_sr', '0'), ('tf2nr_fsr_fg', '0'), ('tf2nr_upscaler', '1')):
                if defaults.get(cv) != want:
                    errors.append(f'{mj} ConVar {cv} DefaultValue {defaults.get(cv)!r} != {want!r}')

    # licences / notices that must travel with the bundled third-party binaries
    for req in ('THIRD_PARTY_NOTICES.txt', 'licenses/NVIDIA_RTX_SDKs_LICENSE.txt', 'licenses/FidelityFX_SDK_LICENSE.txt',
                'licenses/ReShade_LICENSE.txt'):
        if req not in nameset: errors.append(f'{req} missing')
    if 'THIRD_PARTY_NOTICES.txt' in nameset:
        tpn = pkg.read('THIRD_PARTY_NOTICES.txt').decode('utf-8', 'replace')
        for label, phrase in (('ReShade BSD-3 text', 'Redistribution and use in source and binary forms, with or without modification'),
                              ('ReShade copyright', 'Copyright 2014 Patrick Mours'),
                              ('FidelityFX MIT text', 'Permission is hereby granted, free of charge'),
                              ('FidelityFX copyright', 'Advanced Micro Devices, Inc.'),
                              ('NVIDIA SDK notice (RTX SDKs License 2.b)', 'This software contains source code provided by NVIDIA Corporation.'),
                              ('NVIDIA DLSS attribution', 'NVIDIA DLSS'),
                              ('RSA MD5 notice (md5.c in the FidelityFX DX11 backend)', 'RSA Data Security, Inc. MD5 Message Digest Algorithm')):
            if phrase not in tpn: errors.append(f'THIRD_PARTY_NOTICES.txt lacks the {label}')

    # the user-facing text must match the tf2-dlss-sr build that ships: a build without the FSR 3 upscaler logs
    # "no FSR 3 upscaler" (sr 0.3.2); then no text may promise FSR 3 upscaling / an FSR fallback, and the menu must hide it
    sr_name = 'plugins/lib/tf2-dlss-sr.addon64'
    if sr_name in nameset:
        sr_bin = pkg.read(sr_name)
        sr_has_fsr3 = b'no FSR 3 upscaler' not in sr_bin
        notes.append(f'tf2-dlss-sr: FSR 3 upscaler {"present" if sr_has_fsr3 else "ABSENT (DLSS only)"}')
        if not sr_has_fsr3:
            claims = [r'Off\s*/\s*DLSS\s*/\s*FSR\s*3', r'DLSS\s+or\s+(AMD\s+)?FSR\s*3', r'uses\s+FSR\s*3\s+(instead|automatically)',
                      r'FSR\s*3\s*\(upscaling', r'FSR\s*3\s+upscaler\)', r'both\s+upscalers', r'FSR\s*3\s+upscaling\s+and',
                      r'(DLSS|upscaler)\s+(on\s+)?\(DLSS\s+or\s+FSR', r'FSR\s*3\s+Native\s+AA']
            texts = [(n, pkg.read(n).decode('utf-8', 'replace')) for n in ('README.md', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.txt') if n in nameset]
            texts.append(('manifest.json description', manifest.get('description', '') if manifest else ''))
            for n, t in texts:
                for pat in claims:
                    m = re.search(pat, t, re.I)
                    if m: errors.append(f'{n} promises FSR 3 upscaling ({m.group(0)!r}) but the packaged tf2-dlss-sr has no FSR 3 upscaler')
            for n in names:
                if n.startswith('mods/') and n.endswith('.nut'):
                    t = pkg.read(n).decode('utf-8', 'replace')
                    m = re.search(r'const\s+bool\s+TF2NR_FSR3_UPSCALER\s*=\s*(\w+)', t)
                    if m and m.group(1) != 'false':
                        errors.append(f'{n}: TF2NR_FSR3_UPSCALER = {m.group(1)} but the packaged tf2-dlss-sr has no FSR 3 upscaler')

    # nvngx_dlss.dll provenance (1.0.0 review): builds known NOT to come from an NVIDIA SDK release
    nv_name = 'plugins/lib/nvngx_dlss.dll'
    if nv_name in nameset:
        import hashlib
        nv_sha = hashlib.sha256(pkg.read(nv_name)).hexdigest()
        unofficial = {'c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e': '310.8.0.0 from a community DLSS 5 bundle'}
        if nv_sha in unofficial:
            warns.append(f'{nv_name} = {unofficial[nv_sha]} (sha256 {nv_sha[:16]}): NOT an NVIDIA SDK release, so the RTX SDKs '
                         'licence basis for redistributing it is not established - OWNER DECISION BEFORE UPLOAD (README_DEV.md)')
        else:
            notes.append(f'{nv_name} sha256 {nv_sha[:16]}')
    for n in names:
        if n.endswith(('.nut', '.gnut')):
            try:
                errors += lint_nut(n, pkg.read(n).decode('utf-8'))
            except UnicodeDecodeError:
                errors.append(f'{n} is not UTF-8')
        elif n.endswith('.json') and n != 'manifest.json':
            try: json.loads(pkg.read(n).decode('utf-8-sig'))
            except Exception as e: errors.append(f'{n} invalid JSON: {e}')

    # plugins
    top_plugins = [n for n in names if re.fullmatch(r'plugins/[^/]+\.dll', n, re.I)]
    if not top_plugins: errors.append('no plugins/*.dll (the loader plugin)')
    for n in top_plugins:
        try:
            machine, exports = pe_info(pkg.read(n))
            if machine != 0x8664: errors.append(f'{n} is not x64 (machine 0x{machine:x})')
            if 'CreateInterface' not in exports: errors.append(f'{n} does not export CreateInterface (Northstar would unload it)')
            else: notes.append(f'plugin {n}: x64, exports {exports}')
        except ValueError as e:
            errors.append(f'{n}: bad PE: {e}')
    for n in names:
        if n.startswith('plugins/') and not re.fullmatch(r'plugins/[^/]+', n) and not n.startswith('plugins/lib/'):
            warns.append(f'{n}: only plugins/*.dll and plugins/lib/* are meaningful to Northstar')
    for req in REQUIRED_LIB:
        if req not in nameset: errors.append(f'{req} missing')
    for n in names:
        if n.startswith('plugins/lib/') and n.lower().endswith(('.dll', '.addon64')):
            try:
                machine, exports = pe_info(pkg.read(n))
                if machine != 0x8664: errors.append(f'{n} is not x64')
                if n.endswith(('ReShade64.dll', 'proxy/dxgi.dll')) and 'ReShadeRegisterAddon' not in exports: errors.append(f'{n} is not a ReShade add-on build')
            except ValueError as e:
                errors.append(f'{n}: bad PE: {e}')
    for n in names:   # plugins/lib is on the process DLL search path (Northstar AddDllDirectory): no system-DLL names there
        if re.fullmatch(r'plugins/lib/(dxgi|d3d9|d3d10|d3d10_1|d3d11|d3d12|dinput8?|opengl32|winmm|version)\.dll', n, re.I):
            errors.append(f'{n}: a system DLL name directly in plugins/lib would be found by name-based loads (use plugins/lib/proxy/)')
    if 'plugins/lib/ReShade.ini' in nameset:
        ini = pkg.read('plugins/lib/ReShade.ini').decode('utf-8', 'replace')
        # NeuralUplift = renodx's NR on/off state: 0 matches the menu default tf2nr_dlss5_nr 0 (with 1, tf2-dlss-sr would send
        # F6 to renodx at every first start after an install or update)
        for sec, key, want in (('TF2DLSSSR', 'Evaluate', '1'), ('TF2DLSSSR', 'DumpShaders', '0'), ('RenoDX.DLSS5', 'DX11Source', 'native'),
                               ('RenoDX.DLSS5', 'NeuralUplift', '0')):
            m = re.search(r'^\[' + re.escape(sec) + r'\]\s*$(.*?)(?=^\[|\Z)', ini, re.M | re.S)
            val = re.search(r'^' + re.escape(key) + r'=(.*)$', m.group(1), re.M) if m else None
            if not val or val.group(1).strip() != want: errors.append(f'ReShade.ini [{sec}] {key} must be {want}')
        if re.search(r'^\s*(AddonPath|EffectSearchPaths|TextureSearchPaths|PresetPath|IntermediateCachePath)\s*=\s*[A-Za-z]:\\', ini, re.M | re.I):
            errors.append('ReShade.ini contains an absolute (machine-specific) path')

    size = os.path.getsize(path) if not os.path.isdir(path) else sum(os.path.getsize(os.path.join(path, *n.split('/'))) for n in names)
    notes.append(f'{len(names)} files, {size / 1e6:.1f} MB')
    if manifest:
        notes.append(f'installs as R2Northstar\\packages\\<Team>-{manifest.get("name")}-{manifest.get("version_number")}\\')
    if not quiet:
        for n in notes: print('  note :', n)
        for w in warns: print('  WARN :', w)
        for e in errors: print('  ERROR:', e)
        print(('INVALID' if errors else 'VALID') + f': {path} ({len(errors)} errors, {len(warns)} warnings)')
    return 1 if errors else 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__); sys.exit(2)
    sys.exit(validate(sys.argv[1]))
