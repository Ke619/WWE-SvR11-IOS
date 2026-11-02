"""Convert captured shader containers to HLSL (XenosRecomp), DXIL and SPIR-V (DXC).

    python convert_shaders.py [xsc_dir] [out_dir]

Defaults: runs/d3dtrace/xsc -> runs/shaders_native/{hlsl,dxil}. Output per
shader <hash>.<vs|ps> (hash = FNV-1a of the container, as the native renderer
computes it when the game creates the shader):
  dxil/<hash>.<vs|ps>.dxil        specialization constants = 0
  dxil/<hash>.vs.s1.dxil          packed 11:11:10 normals (if the shader has them)
  dxil/<hash>.ps.s2.dxil          alpha test (if the shader has it)
  dxil/<hash>.vs.inputs           the vertex shader's inputs: SEMANTIC INDEX TYPE LOCATION
                                  (LOCATION: the Vulkan input location, -1 if unassigned)
  dxil/<hash>.<vs|ps>.textures    the texture fetch slots it samples: SLOT DIMENSION
                                  (0 2D, 1 3D, 2 cube)
spirv/ has the same files with .spv (Vulkan 1.1 / SPIR-V 1.3: -fvk-invert-y for
vertex shaders, constants read from the frame's upload buffer at offsets in the
push constants - no buffer addresses or 64-bit integers; see shader_common.h).
dxbc/ has them as .dxbc (Direct3D 11: Shader Model 5.0 from fxc, -DSVR_D3D11 -
each fetch slot its own register, see shader_common.h); the HLSL fxc reads
(older syntax) is in hlsl_d3d11/; .dxbc4 is the same as Shader Model 4.0 for
feature level 10_x GPUs (only the 11 interpolators the pixel shaders read:
SM4_INTERPOLATORS).
The specialization constants are baked in with -DSVR_SPEC_CONSTANTS (see the
XenosRecomp patch), so the renderer never links shaders at runtime.
Writes out_dir/report.txt with every failure and its first error.
"""
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
XR = os.path.join(ROOT, "recomp", "XenosRecomp", "build", "XenosRecomp", "XenosRecomp.exe")
HEADER = os.path.join(ROOT, "recomp", "XenosRecomp", "XenosRecomp", "shader_common.h")
# (SVR2011_XENOSRECOMP=<folder>: another XenosRecomp checkout - a branch's own)
if os.environ.get("SVR2011_XENOSRECOMP"):
    _xr = os.environ["SVR2011_XENOSRECOMP"]
    XR = os.path.join(_xr, "build_native", "XenosRecomp", "XenosRecomp.exe")
    HEADER = os.path.join(_xr, "XenosRecomp", "shader_common.h")
DXC = os.path.join(ROOT, "recomp", "XenosRecomp", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
# fxc (Direct3D 11's DXBC): the newest Windows SDK's.
FXC = (sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\fxc.exe")) or [""])[-1]

INPUT_RE = re.compile(r"(?:\[\[vk::location\((\d+)\)\]\] )?in (float4|uint4) i\w+ : ([A-Z]+)(\d+)")
TEXTURE_DEFINE_RE = re.compile(r"#define (\w+)_Texture(2D|3D|Cube)DescriptorIndex g_ResourceIndex\(\d+, (\d+)\)")
TEXTURE_USE_RE = re.compile(r"(\w+)_Texture(2D|3D|Cube)DescriptorIndex")
DIMENSIONS = {"2D": 0, "3D": 1, "Cube": 2}


def compile_dxil(hlsl, dxil, stage, mask):
    r = subprocess.run([DXC, "-nologo", "-HV", "2021", "-T", f"{stage}_6_0", "-E", "main",
                        f"-DSVR_SPEC_CONSTANTS={mask}", "-Fo", dxil, hlsl],
                       capture_output=True, text=True, timeout=120)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


def compile_spirv(hlsl, spv, stage, mask, compact=False):
    flags = ["-fvk-invert-y"] if stage == "vs" else ["-fvk-use-dx-position-w"]
    if compact:
        flags.append("-DSVR_COMPACT_TABLES")
    r = subprocess.run([DXC, "-nologo", "-HV", "2021", "-T", f"{stage}_6_0", "-E", "main", "-spirv",
                        "-fvk-use-dx-layout", "-fspv-target-env=vulkan1.1", *flags,
                        f"-DSVR_SPEC_CONSTANTS={mask}", "-Fo", spv, hlsl],
                       capture_output=True, text=True, timeout=120)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


# fxc reads an older HLSL than DXC: no [[vk::...]] attributes, register
# spaces, [shader()] attributes or empty-parameter macros, and a macro NAME(i)
# can't share its name with the array NAME[] it reads.
ARRAY_MACRO_RE = re.compile(r"#define (\w+)\(INDEX\)")


def fxc_source(text):
    text = re.sub(r"\[\[vk::[^\]]*\]\]", "", text)
    text = text.replace(", space4)", ")")
    text = re.sub(r'\[shader\("\w+"\)\]', "", text)
    text = text.replace("#define DEFINE_SHARED_CONSTANTS() \\", "#define DEFINE_SHARED_CONSTANTS \\")
    text = text.replace("DEFINE_SHARED_CONSTANTS();", "DEFINE_SHARED_CONSTANTS")
    for name in set(ARRAY_MACRO_RE.findall(text)):
        text = re.sub(r"\b%s\[" % re.escape(name), name + "_a[", text)
    return finite_clamps(text)


# The converted shaders keep log2 / rcp / rsqrt results finite as the Xenos
# does: clamp(rsqrt(x), FLT_MIN, FLT_MAX) (FLT_MIN being -FLT_MAX there). fxc
# takes floats as finite and drops such a clamp, so rsqrt(0) stayed infinite and
# a zero vector normalised (0 * inf) was NaN - D3D11 drew the ringside floor and
# the barricade tops white. The clamps go through a function on the float's bits
# (integer operations: nothing fxc or a driver can fold away): the magnitude
# capped at FLT_MAX, the sign kept - +-inf (and NaN) become +-FLT_MAX, finite
# values pass bit for bit. (The first version, a `precise` float clamp, broke
# the wrestlers' skin on Fermi GPUs - GT 430, report 20261102-175019-77d3c2.)
FINITE_HELPER = ("float SvrFinite(float x) { uint u = asuint(x); "
                 "return asfloat((u & 0x80000000u) | min(u & 0x7fffffffu, 0x7f7fffffu)); }\n")
FINITE_TAIL = ", FLT_MIN, FLT_MAX)"


def finite_clamps(text):
    text = finite_calls(text)
    start = text.find("void main(")
    if "SvrFinite(" not in text or start < 0:
        return text
    return text[:start] + FINITE_HELPER + text[start:]


def finite_calls(text):
    out, pos = [], 0
    while True:
        start = text.find("clamp(", pos)
        if start < 0:
            break
        depth, k = 1, start + len("clamp(")
        while k < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[k], 0)
            k += 1
        call = text[start:k]
        if call.endswith(FINITE_TAIL):
            inner = finite_calls(call[len("clamp("):-len(FINITE_TAIL)])
            out.append(text[pos:start] + "SvrFinite(" + inner + ")")
            pos = k
        else:  # (another clamp: its arguments may hold some)
            out.append(text[pos:start + len("clamp(")])
            pos = start + len("clamp(")
    out.append(text[pos:])
    return "".join(out)


# Shader Model 4.0 (feature level 10_x GPUs) has 16 interpolator registers;
# the converted shaders declare 24 (TEXCOORD0-15, COLOR0-7) of which the game's
# pixel shaders read only these. Every shader keeps them in this order (so a
# vertex shader's outputs line up with any pixel shader's inputs) and the rest
# become plain variables.
SM4_INTERPOLATORS = [f"TEXCOORD{i}" for i in range(8)] + ["COLOR0", "COLOR3", "COLOR4"]
PARAM_RE = re.compile(r"^\s*(in|out) float4 (\w+) : ([A-Z]+\d+)$")


def sm4_source(text):
    start = text.find("void main(")
    if start < 0:
        return text, None
    end = text.find(")", start)
    params = text[start + len("void main("):end].split(",")
    kept, dropped = [], []
    for p in params:
        m = PARAM_RE.match(p.strip("\r\n"))
        if m and m.group(3) not in SM4_INTERPOLATORS and m.group(3).startswith(("TEXCOORD", "COLOR")):
            dropped.append(m)
        else:
            kept.append(p)
    body = text[end:]
    for m in dropped:
        if m.group(1) == "in" and re.search(r"\b%s\b" % m.group(2), body):
            return text, f"sm4: reads {m.group(3)}"
    statics = "".join(f"static float4 {m.group(2)} = 0;\n" for m in dropped)
    # (rcp() is Shader Model 5 only)
    return ("#define rcp(X) (1.0 / (X))\n" + text[:start] + statics + "void main(" + ",".join(kept) + body,
            None)


def compile_dxbc(hlsl, dxbc, stage, mask, fixed_dir, model="5_0"):
    fixed = os.path.join(fixed_dir, os.path.basename(hlsl))
    with open(hlsl, encoding="utf-8", errors="ignore") as f:
        text = fxc_source(f.read())
    if model == "4_0":
        fixed = fixed[:-5] + ".sm4.hlsl"
        text, err = sm4_source(text)
        if err:
            return err
    with open(fixed, "w", encoding="utf-8") as f:
        f.write(text)
    r = subprocess.run([FXC, "/nologo", "/T", f"{stage}_{model}", "/E", "main", "/O3", "/DSVR_D3D11",
                        f"/DSVR_SPEC_CONSTANTS={mask}", "/I", os.path.dirname(os.path.abspath(hlsl)),
                        "/Fo", dxbc, fixed],
                       capture_output=True, text=True, timeout=300)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


def convert(xsc, out):
    name = os.path.basename(xsc)[:-4]          # <hash>.<vs|ps>
    stage = "ps" if name.endswith(".ps") else "vs"
    hlsl = os.path.join(out, "hlsl", name + ".hlsl")
    r = subprocess.run([XR, xsc, hlsl, HEADER], capture_output=True, text=True, timeout=60)
    if r.returncode or not os.path.exists(hlsl) or not os.path.getsize(hlsl):
        return name, "hlsl", (r.stderr or r.stdout).strip()[:300]
    src = open(hlsl, encoding="utf-8", errors="ignore").read()
    body = src[src.find("void main("):]
    variants = [0]
    if stage == "vs" and "tfetchR11G11B10(" in body:
        variants.append(1)
    if stage == "ps" and "SPEC_CONSTANT_ALPHA_TEST" in body:
        variants.append(2)
    for mask in variants:
        suffix = "" if mask == 0 else f".s{mask}"
        err = compile_dxil(hlsl, os.path.join(out, "dxil", f"{name}{suffix}.dxil"), stage, mask)
        if err:
            return name, f"dxil s{mask}", err
        err = compile_spirv(hlsl, os.path.join(out, "spirv", f"{name}{suffix}.spv"), stage, mask)
        if err:
            return name, f"spirv s{mask}", err
        # (.spvc: small per-draw tables, for GPUs without descriptor indexing)
        err = compile_spirv(hlsl, os.path.join(out, "spirv", f"{name}{suffix}.spvc"), stage, mask, True)
        if err:
            return name, f"spirv compact s{mask}", err
        if FXC:
            err = compile_dxbc(hlsl, os.path.join(out, "dxbc", f"{name}{suffix}.dxbc"), stage, mask,
                               os.path.join(out, "hlsl_d3d11"))
            if err:
                return name, f"dxbc s{mask}", err
            # (.dxbc4: Shader Model 4.0, for feature level 10_x GPUs)
            err = compile_dxbc(hlsl, os.path.join(out, "dxbc", f"{name}{suffix}.dxbc4"), stage, mask,
                               os.path.join(out, "hlsl_d3d11"), "4_0")
            if err:
                return name, f"dxbc4 s{mask}", err
    slots = {(name, dim): int(slot) for name, dim, slot in TEXTURE_DEFINE_RE.findall(src)}
    used = sorted({(slots[(name, dim)], DIMENSIONS[dim]) for name, dim in TEXTURE_USE_RE.findall(body)
                   if (name, dim) in slots})
    for d in ("dxil", "spirv", "dxbc"):
        with open(os.path.join(out, d, name + ".textures"), "w") as f:
            for slot, dim in used:
                f.write(f"{slot} {dim}\n")
        if stage == "vs":
            header = body[:body.find("{")]
            with open(os.path.join(out, d, name + ".inputs"), "w") as f:
                for location, typ, semantic, index in INPUT_RE.findall(header):
                    f.write(f"{semantic} {index} {typ} {location or -1}\n")
    return name, None, ""


def main():
    xsc_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "port", "runs", "d3dtrace", "xsc")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "port", "runs", "shaders_native")
    for d in ("hlsl", "hlsl_d3d11", "dxil", "spirv", "dxbc"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    files = sorted(glob.glob(os.path.join(xsc_dir, "*.xsc")))
    # (SVR2011_JOBS: how many conversions at once - 2 while the PC is in use)
    with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get("SVR2011_JOBS", "8"))) as pool:
        results = list(pool.map(lambda f: convert(f, out), files))
    # The renderer's own shaders (src/native/shaders/<name>.<vs|ps>.hlsl).
    own = os.path.join(ROOT, "port", "src", "native", "shaders")
    for hlsl in sorted(glob.glob(os.path.join(own, "*.hlsl"))):
        name = os.path.basename(hlsl)[:-5]
        err = compile_dxil(hlsl, os.path.join(out, "dxil", name + ".dxil"), name.split(".")[-1], 0)
        if err:
            results.append((name, "own", err))
        # (the debug shaders but debug_solid are D3D12 only)
        for ext, compact in ((".spv", False), (".spvc", True)):
            err = compile_spirv(hlsl, os.path.join(out, "spirv", name + ext), name.split(".")[-1], 0, compact)
            if err and (not name.startswith("debug_") or name.startswith("debug_solid")):
                results.append((name, "own spirv", err))
        if FXC:
            for ext, model in ((".dxbc", "5_0"), (".dxbc4", "4_0")):
                err = compile_dxbc(hlsl, os.path.join(out, "dxbc", name + ext), name.split(".")[-1], 0,
                                   os.path.join(out, "hlsl_d3d11"), model)
                if err and (not name.startswith("debug_") or name.startswith("debug_solid")):
                    results.append((name, "own " + ext[1:], err))
    # FSR 2's passes (fsr2_shaders.py: DXIL + SPIR-V, Direct3D 12 / Vulkan only).
    import fsr2_shaders
    results += [(name, "fsr2 " + what, err) for name, what, err in fsr2_shaders.build(out)]
    failed = [r for r in results if r[1]]
    with open(os.path.join(out, "report.txt"), "w") as rep:
        rep.write(f"{len(files)} shaders, {len(files) - len(failed)} converted to DXIL, SPIR-V and DXBC\n")
        for name, stage, err in failed:
            rep.write(f"{name}: {stage}: {err}\n")
    vs = sum(1 for f in files if f.endswith(".vs.xsc"))
    variants = len(glob.glob(os.path.join(out, "dxil", "*.s?.dxil")))
    print(f"{len(files)} shaders ({vs} vertex, {len(files) - vs} pixel): "
          f"{len(files) - len(failed)} compiled to DXIL and SPIR-V (+{variants} specialized variants), "
          f"{len(failed)} failed -> {out}")


if __name__ == "__main__":
    main()
