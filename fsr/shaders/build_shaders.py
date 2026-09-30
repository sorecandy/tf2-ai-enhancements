#!/usr/bin/env python3
"""build_shaders.py - compile the FSR 3.1 frame interpolation (FI) + optical flow (OF) compute passes for D3D11
with the local fxc, reflect them with our own tool (tools/ffx_reflect.exe), validate the reflection against what
the patched DX11 backend and the FI/OF components accept, and emit FidelityFX_SC-compatible blob headers.

No FidelityFX_SC.exe (or any other downloaded executable) is run. Inputs come from ../sdk_patched (the patched
copy of the pristine ffx-sdk-dx11 tree). Outputs:
  out/cso/<pass>.cso            DXBC (reflection kept, no /Qstrip_reflect)
  out/asm/<pass>.asm            fxc /Fc listing (review aid; also scanned for typed UAV loads)
  out/reflect/<pass>.json       raw reflection from ffx_reflect.exe
  generated/<pass>_<hash>.h     per-permutation blob + reflection tables (FidelityFX_SC layout)
  generated/<pass>_permutations.h   PermutationKey union, PermutationInfo struct, IndirectionTable, PermutationInfo[]
  generated/shader_manifest.json    summary: UAV slot counts, requires flags, min FL, typed UAV loads per pass
Exit code != 0 on any compile/validation failure.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDK = os.path.join(ROOT, "sdk_patched")
GPU = os.path.join(SDK, "include", "FidelityFX", "gpu")
PASS_DIR_FI = os.path.join(SDK, "src", "backends", "dx12", "shaders", "frameinterpolation")
PASS_DIR_OF = os.path.join(SDK, "src", "backends", "dx12", "shaders", "opticalflow")
FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
REFLECT = os.path.join(HERE, "tools", "ffx_reflect.exe")
OUT = os.path.join(HERE, "out")
GEN = os.path.join(HERE, "generated")

# Base defines: ffx_dx11_fi.md section 7 (== sdk/src/backends/dx11/CMakeLists.txt compile rule for the Wave32
# non-16bit header: -DFFX_HALF=0 -T cs_5_0 -DFFX_HLSL_SM=50, plus the DX12_BASE_ARGS -DFFX_HLSL=1 -DFFX_HLSL_5_0=1
# -DFFX_SPD_NO_WAVE_OPERATIONS and BASE_ARGS -DFFX_GPU=1).
BASE_DEFINES = [
    "FFX_GPU=1",
    "FFX_HLSL=1",
    "FFX_HLSL_5_0=1",
    "FFX_HLSL_SM=50",
    "FFX_HALF=0",
    "FFX_SPD_NO_WAVE_OPERATIONS",
]
# Legacy FSR2-era FI base args from CMakeShadersFrameinterpolation.txt:22-29 (unused by the FI sources; copied so
# the define set matches the fork's own build exactly).
FI_LEGACY_DEFINES = [
    "FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "FFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
]
# Permutation options in the order FidelityFX_SC would emit the key bitfield (CMake PERMUTATION_ARGS order).
FI_OPTIONS = [
    ("FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS", 1),
    ("FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS", 0),
    ("FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH", 0),
]
OF_OPTIONS = [
    ("FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT", 0),
]

FI_PASSES = [
    "ffx_frameinterpolation_setup_pass",
    "ffx_frameinterpolation_reconstruct_and_dilate_pass",
    "ffx_frameinterpolation_reconstruct_previous_depth_pass",
    "ffx_frameinterpolation_game_motion_vector_field_pass",
    "ffx_frameinterpolation_optical_flow_vector_field_pass",
    "ffx_frameinterpolation_disocclusion_mask_pass",
    "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass",
    "ffx_frameinterpolation_compute_inpainting_pyramid_pass",
    "ffx_frameinterpolation_inpainting_pass",
    "ffx_frameinterpolation_debug_view_pass",
    "ffx_frameinterpolation_pass",
]
OF_PASSES = [
    "ffx_opticalflow_prepare_luma_pass",
    "ffx_opticalflow_compute_luminance_pyramid_pass",
    "ffx_opticalflow_generate_scd_histogram_pass",
    "ffx_opticalflow_compute_scd_divergence_pass",
    "ffx_opticalflow_compute_optical_flow_advanced_pass_v5",
    "ffx_opticalflow_filter_optical_flow_pass_v5",
    "ffx_opticalflow_scale_optical_flow_advanced_pass_v5",
]

# Resource classes exactly as FidelityFX_SC's FXC reflection sorts them (hlsl_compiler.cpp ExtractFXCReflectionData):
#   CBUFFER -> CBV, TEXTURE -> TextureSRV, UAV_RWTYPED -> TextureUAV, STRUCTURED -> BufferSRV,
#   UAV_RWSTRUCTURED -> BufferUAV, SAMPLER -> Sampler. Anything else is rejected (the backend cannot bind it).
CLASS_OF_TYPE = {
    "CBUFFER": "CBV",
    "TEXTURE": "TextureSRV",
    "UAV_RWTYPED": "TextureUAV",
    "STRUCTURED": "BufferSRV",
    "UAV_RWSTRUCTURED": "BufferUAV",
    "SAMPLER": "Sampler",
}
CLASS_ORDER = ["CBV", "TextureSRV", "TextureUAV", "BufferSRV", "BufferUAV", "Sampler", "RTAccelerationStructure"]
# PermutationInfo member prefixes, in POPULATE_SHADER_BLOB_FFX order.
CLASS_MEMBER = {
    "CBV": ("numConstantBuffers", "constantBuffer"),
    "TextureSRV": ("numSRVTextures", "srvTexture"),
    "TextureUAV": ("numUAVTextures", "uavTexture"),
    "BufferSRV": ("numSRVBuffers", "srvBuffer"),
    "BufferUAV": ("numUAVBuffers", "uavBuffer"),
    "Sampler": ("numSamplers", "sampler"),
    "RTAccelerationStructure": ("numRTAccelerationStructures", "rtAccelerationStructure"),
}

# Backend limits (sdk_patched ffx_types.h / ffx_dx11.cpp).
FFX_MAX_NUM_SRVS = 16
FFX_MAX_NUM_UAVS = 16
FFX_MAX_NUM_CONST_BUFFERS = 3
FFX_MAX_SAMPLERS = 16
FL11_0_UAV_SLOTS = 8  # d3d11.h D3D11_PS_CS_UAV_REGISTER_COUNT


def write_text(path, text):
    """UTF-8 encode first, write a temp file, then os.replace (Windows-safe, never truncates the target)."""
    data = text.encode("utf-8")
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def log(msg):
    print(msg, flush=True)


def binding_names(component_cpp):
    """Names the component's patchResourceBindings() accepts (L"..." entries of its binding tables)."""
    with open(component_cpp, "rb") as f:
        src = f.read().decode("utf-8", "replace")
    tables = {}
    for m in re.finditer(r"static const (?:ResourceBinding|Binding)\s+(\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
        tables[m.group(1)] = set(re.findall(r'L"([^"]+)"', m.group(2)))
    return tables


def compile_pass(name, src_dir, include_subdir, options, extra_defines):
    src = os.path.join(src_dir, name + ".hlsl")
    cso = os.path.join(OUT, "cso", name + ".cso")
    asm = os.path.join(OUT, "asm", name + ".asm")
    defines = BASE_DEFINES + extra_defines + ["%s=%d" % (o, v) for o, v in options]
    cmd = [FXC, "/nologo", "/T", "cs_5_0", "/E", "CS", "/O3"]
    for d in defines:
        cmd += ["/D", d]
    cmd += ["/I", GPU, "/I", os.path.join(GPU, include_subdir), "/Fo", cso, "/Fc", asm, src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    warn = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or not os.path.isfile(cso):
        log(warn)
        raise SystemExit("fxc failed for %s (exit %d)" % (name, r.returncode))
    # the command line recorded in the generated header (paths relative to the repo root)
    rel = [os.path.relpath(c, ROOT) if os.path.isabs(c) and c != FXC else c for c in cmd]
    rel[0] = "fxc.exe"
    return cso, asm, defines, " ".join(rel), warn


def reflect(cso):
    r = subprocess.run([REFLECT, cso], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("ffx_reflect failed for %s: %s" % (cso, r.stderr.strip()))
    return json.loads(r.stdout)


def typed_uav_loads(asm_path, refl):
    """UAV names read with ld_uav_typed (GetRequiresFlags misses 8/16-bit formats declared as 32-bit HLSL types)."""
    with open(asm_path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    slots = set(int(m.group(1)) for m in re.finditer(r"^\s*ld_uav_typed\S*\s+[^,]+,\s*[^,]+,\s*u(\d+)", text, re.M))
    uav_by_slot = {r["bind"]: r["name"] for r in refl["resources"] if r["type"].startswith("UAV")}
    return sorted(uav_by_slot.get(s, "u%d" % s) for s in slots)


def classify(name, refl, tables, effect):
    classes = {c: [] for c in CLASS_ORDER}
    errors = []
    for r in refl["resources"]:
        t = r["type"]
        if t not in CLASS_OF_TYPE:
            errors.append("%s: resource %s has unsupported type %s" % (name, r["name"], t))
            continue
        if r["count"] != 1:
            # the DX11 backend's SRV/UAV array paths index job arrays inconsistently; we only accept scalars
            errors.append("%s: resource %s has bind count %d (only 1 supported)" % (name, r["name"], r["count"]))
        classes[CLASS_OF_TYPE[t]].append({"name": r["name"], "binding": r["bind"], "count": r["count"], "space": 0,
                                          "stride": r["samples"] if t in ("STRUCTURED", "UAV_RWSTRUCTURED") else 0})
    for c in classes:
        classes[c].sort(key=lambda e: e["binding"])

    # constant buffers: the backend binds CB i at slot i (ffx_dx11.cpp CSSetConstantBuffers(index)), so the
    # reflected bind points must be exactly 0..n-1 after sorting.
    for i, cb in enumerate(classes["CBV"]):
        if cb["binding"] != i:
            errors.append("%s: constant buffer %s at b%d, expected b%d (CBs must be contiguous from b0)"
                          % (name, cb["name"], cb["binding"], i))
    if len(classes["CBV"]) > FFX_MAX_NUM_CONST_BUFFERS:
        errors.append("%s: %d constant buffers > FFX_MAX_NUM_CONST_BUFFERS" % (name, len(classes["CBV"])))
    if len(classes["TextureSRV"]) + 0 > FFX_MAX_NUM_SRVS or len(classes["BufferSRV"]) > FFX_MAX_NUM_SRVS:
        errors.append("%s: too many SRVs" % name)
    if len(classes["TextureUAV"]) > FFX_MAX_NUM_UAVS or len(classes["BufferUAV"]) > FFX_MAX_NUM_UAVS:
        errors.append("%s: too many UAVs" % name)
    if len(classes["Sampler"]) > FFX_MAX_SAMPLERS:
        errors.append("%s: too many samplers" % name)
    for s in classes["Sampler"]:
        if s["binding"] >= 2:
            errors.append("%s: sampler %s at s%d, but the components only create 2 samplers" % (name, s["name"], s["binding"]))

    # names must be known to the component's patchResourceBindings() (it looks buffers up in the same tables)
    if effect == "FI":
        srv_t, uav_t, cb_t = tables["srvResourceBindingTable"], tables["uavResourceBindingTable"], tables["cbResourceBindingTable"]
    else:
        srv_t, uav_t, cb_t = tables["srvBindingNames"], tables["uavBindingNames"], tables["cbBindingNames"]
    for e in classes["TextureSRV"] + classes["BufferSRV"]:
        if e["name"] not in srv_t:
            errors.append("%s: SRV name %s not in the component binding table" % (name, e["name"]))
    for e in classes["TextureUAV"] + classes["BufferUAV"]:
        if e["name"] not in uav_t:
            errors.append("%s: UAV name %s not in the component binding table" % (name, e["name"]))
    for e in classes["CBV"]:
        if e["name"] not in cb_t:
            errors.append("%s: CB name %s not in the component binding table" % (name, e["name"]))
    if effect == "OF" and (classes["BufferSRV"] or classes["BufferUAV"]):
        # the OF component's patchResourceBindings() never maps buffer bindings
        errors.append("%s: OF pass uses buffer bindings, which the OF component cannot map" % name)
    return classes, errors


def emit_headers(name, refl, classes, options, cmdline, cso_path):
    with open(cso_path, "rb") as f:
        blob = f.read()
    perm = "%s_%s" % (name, refl["hash"])
    L = []
    L.append("// %s.h." % perm)
    L.append("// Auto generated by tf2-fsr-fg shaders/build_shaders.py (FidelityFX_SC-compatible layout; FidelityFX_SC not used).")
    L.append("// " + cmdline)
    L.append("// reflection: requires=%s min_fl=%s thread_group=%s" % (refl["requires"], refl["min_fl"], refl["tg"]))
    L.append("")
    for c in CLASS_ORDER:
        entries = classes.get(c, [])
        if not entries:
            continue
        L.append("static const char* g_%s_%sResourceNames[] = { %s };" % (perm, c, " ".join('"%s",' % e["name"] for e in entries)))
        L.append("static const uint32_t g_%s_%sResourceBindings[] = { %s };" % (perm, c, " ".join("%d," % e["binding"] for e in entries)))
        L.append("static const uint32_t g_%s_%sResourceCounts[] = { %s };" % (perm, c, " ".join("%d," % e["count"] for e in entries)))
        L.append("static const uint32_t g_%s_%sResourceSpaces[] = { %s };" % (perm, c, " ".join("%d," % e["space"] for e in entries)))
        L.append("")
    L.append("static const uint32_t g_%s_size = %d;" % (perm, len(blob)))
    L.append("")
    L.append("static const unsigned char g_%s_data[] = {" % perm)
    rows = []
    for i in range(0, len(blob), 16):
        rows.append(",".join("0x%02x" % b for b in blob[i:i + 16]))
    L.append(",\n".join(rows))
    L.append("};")
    L.append("")
    write_text(os.path.join(GEN, perm + ".h"), "\n".join(L))

    # permutations header
    P = []
    P.append('#include "%s.h"' % perm)
    P.append("")
    P.append("typedef union %s_PermutationKey {" % name)
    P.append("    struct {")
    for o, _ in options:
        P.append("        uint32_t %s : 1;" % o)
    P.append("    };")
    P.append("    uint32_t index;")
    P.append("} %s_PermutationKey;" % name)
    P.append("")
    P.append("typedef struct %s_PermutationInfo {" % name)
    P.append("    const uint32_t       blobSize;")
    P.append("    const unsigned char* blobData;")
    P.append("")
    for c in CLASS_ORDER:
        num, pre = CLASS_MEMBER[c]
        P.append("    const uint32_t  %s;" % num)
        P.append("    const char**    %sNames;" % pre)
        P.append("    const uint32_t* %sBindings;" % pre)
        P.append("    const uint32_t* %sCounts;" % pre)
        P.append("    const uint32_t* %sSpaces;" % pre)
        P.append("")
    P.append("} %s_PermutationInfo;" % name)
    P.append("")
    key = 0
    for bit, (o, v) in enumerate(options):
        key |= (v & 1) << bit
    n = 1 << len(options)
    # FidelityFX_SC maps keys it did not compile to entry 0; we map them to -1 so the accessor returns an empty
    # blob and pipeline creation fails loudly instead of silently running the wrong permutation.
    P.append("// only key %d (%s) is compiled; -1 = permutation not built" % (key, ", ".join("%s=%d" % (o, v) for o, v in options)))
    P.append("static const int32_t g_%s_IndirectionTable[] = {" % name)
    for i in range(n):
        P.append("    %d," % (0 if i == key else -1))
    P.append("};")
    P.append("")
    P.append("static const uint32_t g_%s_PermutationCount = 1;" % name)
    P.append("")
    parts = []
    for c in CLASS_ORDER:
        entries = classes.get(c, [])
        if entries:
            parts.append("%d, g_%s_%sResourceNames, g_%s_%sResourceBindings, g_%s_%sResourceCounts, g_%s_%sResourceSpaces"
                         % (len(entries), perm, c, perm, c, perm, c, perm, c))
        else:
            parts.append("0, 0, 0, 0, 0")
    P.append("static const %s_PermutationInfo g_%s_PermutationInfo[] = {" % (name, name))
    P.append("    { g_%s_size, g_%s_data, %s }," % (perm, perm, ", ".join(parts)))
    P.append("};")
    P.append("")
    write_text(os.path.join(GEN, name + "_permutations.h"), "\n".join(P))
    return perm


def main():
    for d in (os.path.join(OUT, "cso"), os.path.join(OUT, "asm"), os.path.join(OUT, "reflect"), GEN):
        os.makedirs(d, exist_ok=True)
    if not os.path.isfile(FXC):
        raise SystemExit("fxc not found: " + FXC)
    if not os.path.isfile(REFLECT):
        raise SystemExit("ffx_reflect.exe not built (run tools/build_reflect.bat)")
    # stale generated headers would be picked up by the accessors; start clean
    for f in os.listdir(GEN):
        if f.endswith(".h") or f.endswith(".json"):
            os.remove(os.path.join(GEN, f))

    fi_tables = binding_names(os.path.join(SDK, "src", "components", "frameinterpolation", "ffx_frameinterpolation.cpp"))
    of_tables = binding_names(os.path.join(SDK, "src", "components", "opticalflow", "ffx_opticalflow.cpp"))

    jobs = [(p, PASS_DIR_FI, "frameinterpolation", FI_OPTIONS, FI_LEGACY_DEFINES, "FI", fi_tables) for p in FI_PASSES]
    jobs += [(p, PASS_DIR_OF, "opticalflow", OF_OPTIONS, [], "OF", of_tables) for p in OF_PASSES]

    manifest = {"fxc": FXC, "base_defines": BASE_DEFINES, "fi_extra_defines": FI_LEGACY_DEFINES,
                "fi_options": FI_OPTIONS, "of_options": OF_OPTIONS, "passes": []}
    all_errors = []
    for name, src_dir, sub, options, extra, effect, tables in jobs:
        cso, asm, defines, cmdline, warn = compile_pass(name, src_dir, sub, options, extra)
        refl = reflect(cso)
        write_text(os.path.join(OUT, "reflect", name + ".json"), json.dumps(refl, indent=1))
        classes, errors = classify(name, refl, tables, effect)
        all_errors += errors
        uavs = [r for r in refl["resources"] if r["type"].startswith("UAV")]
        uav_slots = (max(r["bind"] + r["count"] for r in uavs)) if uavs else 0
        loads = typed_uav_loads(asm, refl)
        perm = emit_headers(name, refl, classes, options, cmdline, cso)
        req = int(refl["requires"], 16)
        entry = {
            "pass": name, "effect": effect, "permutation": perm, "dxbc_bytes": refl["size"],
            "requires": refl["requires"], "min_feature_level": refl["min_fl"], "thread_group": refl["tg"],
            "uav_slots_used": uav_slots, "needs_fl11_1": uav_slots > FL11_0_UAV_SLOTS or (req & 0x8) != 0,
            "typed_uav_load_flag": (req & 0x800) != 0, "typed_uav_loads_in_asm": loads,
            "classes": {c: [(e["name"], e["binding"]) for e in classes[c]] for c in CLASS_ORDER if classes[c]},
            "fxc_warnings": len([l for l in warn.splitlines() if "warning" in l]),
        }
        manifest["passes"].append(entry)
        log("%-72s %6d B  uav_slots=%-2d req=%-6s minFL=%s typed_loads=%s"
            % (name, refl["size"], uav_slots, refl["requires"], refl["min_fl"], ",".join(loads) or "-"))

    write_text(os.path.join(GEN, "shader_manifest.json"), json.dumps(manifest, indent=1))
    if all_errors:
        for e in all_errors:
            log("ERROR: " + e)
        raise SystemExit("%d validation error(s)" % len(all_errors))
    n11_1 = [p["pass"] for p in manifest["passes"] if p["needs_fl11_1"]]
    log("OK: %d passes compiled + validated. Passes needing FL 11_1 (>8 UAV slots): %s" % (len(manifest["passes"]), ", ".join(n11_1) or "none"))
    return 0



# ---------------------------------------------------------------------------------------------------------------------
# FSR 3.1 upscaler (tf2-dlss-sr's second backend, 2026-09-29). `python build_shaders.py fsr3up` builds ONLY these passes
# into shaders/generated_fsr3up (the FI/OF output in shaders/generated is not touched). Sources: sdk_patched/src/backends/
# dx11/shaders/fsr3upscaler (the fork's DX11 pass files, copied unchanged from the pristine tree). Defines = the fork's
# CMakeShadersFSR3Upscaler.txt FSR3UPSCALER_BASE_ARGS + DX12_BASE_ARGS (-DFFX_HLSL=1 -DFFX_HLSL_5_0=1
# -DFFX_SPD_NO_WAVE_OPERATIONS) on cs_5_0 / FFX_HALF=0, like FI/OF.
# Permutation keys compiled (the only ones tf2-dlss-sr creates; the DX11 backend reports no wave lanes and no FP16, so
# REPROJECT_USE_LANCZOS_TYPE = 0 and there are no 16-bit / wave64 tables): HDR colour 1, render-res MVs 1, jitter-cancel 0,
# inverted depth 0; APPLY_SHARPENING 0 for every pass + 1 for the accumulate pass (ACCUMULATE_SHARPEN).
GEN_FSR3 = os.path.join(HERE, "generated_fsr3up")
OUT_FSR3 = os.path.join(HERE, "out_fsr3up")
PASS_DIR_FSR3 = os.path.join(SDK, "src", "backends", "dx11", "shaders", "fsr3upscaler")
FSR3_LEGACY_DEFINES = [
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
]
FSR3_OPTION_NAMES = [
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH",
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING",
]
FSR3_BASE_KEY = {"FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE": 0, "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT": 1,
                 "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": 1, "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": 0,
                 "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": 0, "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING": 0}
FSR3_PASSES = [
    "ffx_fsr3upscaler_prepare_inputs_pass",
    "ffx_fsr3upscaler_luma_pyramid_pass",
    "ffx_fsr3upscaler_shading_change_pyramid_pass",
    "ffx_fsr3upscaler_shading_change_pass",
    "ffx_fsr3upscaler_prepare_reactivity_pass",
    "ffx_fsr3upscaler_luma_instability_pass",
    "ffx_fsr3upscaler_accumulate_pass",
    "ffx_fsr3upscaler_rcas_pass",
    "ffx_fsr3upscaler_autogen_reactive_pass",
    "ffx_fsr3upscaler_debug_view_pass",
]


def fsr3_keys(name):
    keys = [dict(FSR3_BASE_KEY)]
    if name == "ffx_fsr3upscaler_accumulate_pass":
        k = dict(FSR3_BASE_KEY)
        k["FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING"] = 1
        keys.append(k)
    return keys


def fsr3_compile(name, key, idx):
    src = os.path.join(PASS_DIR_FSR3, name + ".hlsl")
    tag = "%s_k%d" % (name, idx)
    cso = os.path.join(OUT_FSR3, "cso", tag + ".cso")
    asm = os.path.join(OUT_FSR3, "asm", tag + ".asm")
    defines = BASE_DEFINES + FSR3_LEGACY_DEFINES + ["%s=%d" % (o, key[o]) for o in FSR3_OPTION_NAMES]
    cmd = [FXC, "/nologo", "/T", "cs_5_0", "/E", "CS", "/O3"]
    for d in defines:
        cmd += ["/D", d]
    cmd += ["/I", GPU, "/I", os.path.join(GPU, "fsr3upscaler"), "/Fo", cso, "/Fc", asm, src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    warn = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or not os.path.isfile(cso):
        log(warn)
        raise SystemExit("fxc failed for %s (exit %d)" % (tag, r.returncode))
    rel = [os.path.relpath(c, ROOT) if os.path.isabs(c) and c != FXC else c for c in cmd]
    rel[0] = "fxc.exe"
    return cso, asm, " ".join(rel), warn


def fsr3_classify(name, refl, tables):
    classes = {c: [] for c in CLASS_ORDER}
    errors = []
    for r in refl["resources"]:
        t = r["type"]
        if t not in CLASS_OF_TYPE:
            errors.append("%s: resource %s has unsupported type %s" % (name, r["name"], t))
            continue
        if r["count"] != 1:
            errors.append("%s: resource %s has bind count %d (only 1 supported)" % (name, r["name"], r["count"]))
        classes[CLASS_OF_TYPE[t]].append({"name": r["name"], "binding": r["bind"], "count": r["count"], "space": 0,
                                          "stride": r["samples"] if t in ("STRUCTURED", "UAV_RWSTRUCTURED") else 0})
    for c in classes:
        classes[c].sort(key=lambda e: e["binding"])
    for i, cb in enumerate(classes["CBV"]):
        if cb["binding"] != i:
            errors.append("%s: constant buffer %s at b%d, expected b%d (CBs must be contiguous from b0)" % (name, cb["name"], cb["binding"], i))
    if len(classes["CBV"]) > FFX_MAX_NUM_CONST_BUFFERS:
        errors.append("%s: %d constant buffers > FFX_MAX_NUM_CONST_BUFFERS" % (name, len(classes["CBV"])))
    if len(classes["TextureSRV"]) > FFX_MAX_NUM_SRVS or len(classes["TextureUAV"]) > FFX_MAX_NUM_UAVS:
        errors.append("%s: too many SRVs/UAVs" % name)
    if classes["BufferSRV"] or classes["BufferUAV"]:
        errors.append("%s: buffer bindings (the FSR3 upscaler component maps textures only)" % name)
    for s in classes["Sampler"]:
        if s["binding"] >= 2:
            errors.append("%s: sampler %s at s%d, but the component only creates 2 samplers" % (name, s["name"], s["binding"]))
    srv_t, uav_t, cb_t = tables["srvTextureBindingTable"], tables["uavTextureBindingTable"], tables["constantBufferBindingTable"]
    for e in classes["TextureSRV"]:
        if e["name"] not in srv_t:
            errors.append("%s: SRV name %s not in the component binding table" % (name, e["name"]))
    for e in classes["TextureUAV"]:
        if e["name"] not in uav_t:
            errors.append("%s: UAV name %s not in the component binding table" % (name, e["name"]))
    for e in classes["CBV"]:
        if e["name"] not in cb_t:
            errors.append("%s: CB name %s not in the component binding table" % (name, e["name"]))
    return classes, errors


def fsr3_emit(name, perms):
    """perms: list of (key dict, refl, classes, cmdline, cso). One header per permutation + <name>_permutations.h with
    an IndirectionTable over all 64 keys (-1 = not built)."""
    P = []
    perm_names = []
    for key, refl, classes, cmdline, cso in perms:
        with open(cso, "rb") as f:
            blob = f.read()
        perm = "%s_%s" % (name, refl["hash"])
        perm_names.append(perm)
        L = ["// %s.h." % perm,
             "// Auto generated by tf2-fsr-fg shaders/build_shaders.py fsr3up (FidelityFX_SC-compatible layout; FidelityFX_SC not used).",
             "// " + cmdline,
             "// reflection: requires=%s min_fl=%s thread_group=%s" % (refl["requires"], refl["min_fl"], refl["tg"]), ""]
        for c in CLASS_ORDER:
            entries = classes.get(c, [])
            if not entries:
                continue
            L.append("static const char* g_%s_%sResourceNames[] = { %s };" % (perm, c, " ".join('"%s",' % e["name"] for e in entries)))
            L.append("static const uint32_t g_%s_%sResourceBindings[] = { %s };" % (perm, c, " ".join("%d," % e["binding"] for e in entries)))
            L.append("static const uint32_t g_%s_%sResourceCounts[] = { %s };" % (perm, c, " ".join("%d," % e["count"] for e in entries)))
            L.append("static const uint32_t g_%s_%sResourceSpaces[] = { %s };" % (perm, c, " ".join("%d," % e["space"] for e in entries)))
            L.append("")
        L.append("static const uint32_t g_%s_size = %d;" % (perm, len(blob)))
        L.append("")
        L.append("static const unsigned char g_%s_data[] = {" % perm)
        L.append(",\n".join(",".join("0x%02x" % b for b in blob[i:i + 16]) for i in range(0, len(blob), 16)))
        L.append("};")
        L.append("")
        write_text(os.path.join(GEN_FSR3, perm + ".h"), "\n".join(L))
        P.append('#include "%s.h"' % perm)
    P.append("")
    P.append("typedef union %s_PermutationKey {" % name)
    P.append("    struct {")
    for o in FSR3_OPTION_NAMES:
        P.append("        uint32_t %s : 1;" % o)
    P.append("    };")
    P.append("    uint32_t index;")
    P.append("} %s_PermutationKey;" % name)
    P.append("")
    P.append("typedef struct %s_PermutationInfo {" % name)
    P.append("    const uint32_t       blobSize;")
    P.append("    const unsigned char* blobData;")
    P.append("")
    for c in CLASS_ORDER:
        num, pre = CLASS_MEMBER[c]
        P.append("    const uint32_t  %s;" % num)
        P.append("    const char**    %sNames;" % pre)
        P.append("    const uint32_t* %sBindings;" % pre)
        P.append("    const uint32_t* %sCounts;" % pre)
        P.append("    const uint32_t* %sSpaces;" % pre)
        P.append("")
    P.append("} %s_PermutationInfo;" % name)
    P.append("")
    table = [-1] * (1 << len(FSR3_OPTION_NAMES))
    descr = []
    for i, (key, refl, classes, cmdline, cso) in enumerate(perms):
        k = 0
        for bit, o in enumerate(FSR3_OPTION_NAMES):
            k |= (key[o] & 1) << bit
        table[k] = i
        descr.append("key %d = entry %d" % (k, i))
    P.append("// compiled: %s; -1 = permutation not built" % ", ".join(descr))
    P.append("static const int32_t g_%s_IndirectionTable[] = {" % name)
    for v in table:
        P.append("    %d," % v)
    P.append("};")
    P.append("")
    P.append("static const uint32_t g_%s_PermutationCount = %d;" % (name, len(perms)))
    P.append("")
    P.append("static const %s_PermutationInfo g_%s_PermutationInfo[] = {" % (name, name))
    for (key, refl, classes, cmdline, cso), perm in zip(perms, perm_names):
        parts = []
        for c in CLASS_ORDER:
            entries = classes.get(c, [])
            if entries:
                parts.append("%d, g_%s_%sResourceNames, g_%s_%sResourceBindings, g_%s_%sResourceCounts, g_%s_%sResourceSpaces"
                             % (len(entries), perm, c, perm, c, perm, c, perm, c))
            else:
                parts.append("0, 0, 0, 0, 0")
        P.append("    { g_%s_size, g_%s_data, %s }," % (perm, perm, ", ".join(parts)))
    P.append("};")
    P.append("")
    write_text(os.path.join(GEN_FSR3, name + "_permutations.h"), "\n".join(P))
    return perm_names


def main_fsr3up():
    for d in (os.path.join(OUT_FSR3, "cso"), os.path.join(OUT_FSR3, "asm"), os.path.join(OUT_FSR3, "reflect"), GEN_FSR3):
        os.makedirs(d, exist_ok=True)
    if not os.path.isfile(FXC):
        raise SystemExit("fxc not found: " + FXC)
    if not os.path.isfile(REFLECT):
        raise SystemExit("ffx_reflect.exe not built (run tools/build_reflect.bat)")
    for f in os.listdir(GEN_FSR3):
        if f.endswith(".h") or f.endswith(".json"):
            os.remove(os.path.join(GEN_FSR3, f))
    tables = binding_names(os.path.join(SDK, "src", "components", "fsr3upscaler", "ffx_fsr3upscaler.cpp"))
    manifest = {"fxc": FXC, "base_defines": BASE_DEFINES, "fsr3_extra_defines": FSR3_LEGACY_DEFINES, "fsr3_base_key": FSR3_BASE_KEY, "passes": []}
    all_errors = []
    for name in FSR3_PASSES:
        perms = []
        for idx, key in enumerate(fsr3_keys(name)):
            cso, asm, cmdline, warn = fsr3_compile(name, key, idx)
            refl = reflect(cso)
            write_text(os.path.join(OUT_FSR3, "reflect", "%s_k%d.json" % (name, idx)), json.dumps(refl, indent=1))
            classes, errors = fsr3_classify(name, refl, tables)
            all_errors += errors
            uavs = [r for r in refl["resources"] if r["type"].startswith("UAV")]
            uav_slots = (max(r["bind"] + r["count"] for r in uavs)) if uavs else 0
            loads = typed_uav_loads(asm, refl)
            req = int(refl["requires"], 16)
            manifest["passes"].append({
                "pass": name, "key": key, "dxbc_bytes": refl["size"], "requires": refl["requires"], "min_feature_level": refl["min_fl"],
                "thread_group": refl["tg"], "uav_slots_used": uav_slots, "needs_fl11_1": uav_slots > FL11_0_UAV_SLOTS or (req & 0x8) != 0,
                "typed_uav_load_flag": (req & 0x800) != 0, "typed_uav_loads_in_asm": loads,
                "classes": {c: [(e["name"], e["binding"]) for e in classes[c]] for c in CLASS_ORDER if classes[c]},
                "fxc_warnings": len([l for l in warn.splitlines() if "warning" in l])})
            log("%-58s k%d %6d B  uav_slots=%-2d req=%-6s minFL=%s typed_loads=%s"
                % (name, idx, refl["size"], uav_slots, refl["requires"], refl["min_fl"], ",".join(loads) or "-"))
            perms.append((key, refl, classes, cmdline, cso))
        fsr3_emit(name, perms)
    write_text(os.path.join(GEN_FSR3, "shader_manifest.json"), json.dumps(manifest, indent=1))
    if all_errors:
        for e in all_errors:
            log("ERROR: " + e)
        raise SystemExit("%d validation error(s)" % len(all_errors))
    n11_1 = sorted(set(p["pass"] for p in manifest["passes"] if p["needs_fl11_1"]))
    log("OK: %d FSR3 upscaler permutations compiled + validated. Passes needing FL 11_1 (>8 UAV slots): %s" % (len(manifest["passes"]), ", ".join(n11_1) or "none"))
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "fsr3up":
        sys.exit(main_fsr3up())
    sys.exit(main())

