#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The runtime's device profile (tools/sweetmtl/profile/a540-gallium.json): M0's profile
(tools/sweetmtl/m0/a540-gallium.json: vk12-floor plus what Gallium a5xx provides or emulates) with its
format table replaced by what freedreno a5xx does with each format (gen-formats.c's lines).

    make-profile.py M0_PROFILE GEN_FORMATS_OUTPUT VULKAN_CORE_H OUT

M0's table is Vulkan 1.2's minimum, which leaves out formats the a540 renders (Depth32Float, Depth24Stencil8,
the 4:4:4:4 and 1:5:5:5 colors, ...) and claims none of its compressed ones (ETC2, ASTC).
"""
import json
import re
import sys

m0, gen, header, out = sys.argv[1:5]
profile = json.load(open(m0))

names = {}
for m in re.finditer(r"^\s*(VK_FORMAT_[A-Za-z0-9_]+)\s*=\s*(\d+),", open(header).read(), re.M):
    names.setdefault(int(m.group(2)), m.group(1))

SAMPLED, FILTER, STORAGE, ATOMIC, COLOR, BLEND, DEPTH, LINEAR, VERTEX, UTEXEL, STEXEL = (
    0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400)

def bits(features, table):
    return ["VK_FORMAT_FEATURE_" + name for bit, name in table if features & bit]

optimal = [(SAMPLED, "SAMPLED_IMAGE_BIT"), (STORAGE, "STORAGE_IMAGE_BIT"), (ATOMIC, "STORAGE_IMAGE_ATOMIC_BIT"),
           (COLOR, "COLOR_ATTACHMENT_BIT"), (BLEND, "COLOR_ATTACHMENT_BLEND_BIT"),
           (DEPTH, "DEPTH_STENCIL_ATTACHMENT_BIT"), (SAMPLED, "BLIT_SRC_BIT"), (COLOR | DEPTH, "BLIT_DST_BIT"),
           (FILTER, "SAMPLED_IMAGE_FILTER_LINEAR_BIT")]
buffer = [(VERTEX, "VERTEX_BUFFER_BIT"), (UTEXEL, "UNIFORM_TEXEL_BUFFER_BIT"), (STEXEL, "STORAGE_TEXEL_BUFFER_BIT")]

formats = {}
for line in open(gen):
    if line.startswith("#") or not line.strip():
        continue
    vk, features = (int(x) for x in line.split())
    if not features or vk not in names:
        continue
    props = {}
    opt = bits(features, optimal)
    if opt:
        opt += ["VK_FORMAT_FEATURE_TRANSFER_SRC_BIT", "VK_FORMAT_FEATURE_TRANSFER_DST_BIT"]
        props["optimalTilingFeatures"] = sorted(set(opt))
    if features & LINEAR:
        lin = bits(features & (SAMPLED | FILTER | COLOR | BLEND), optimal[:1] + optimal[3:5] + optimal[6:])
        props["linearTilingFeatures"] = sorted(set(lin + ["VK_FORMAT_FEATURE_TRANSFER_SRC_BIT",
                                                          "VK_FORMAT_FEATURE_TRANSFER_DST_BIT"]))
    buf = bits(features, buffer)
    if buf:
        props["bufferFeatures"] = buf
    if props:
        formats[names[vk]] = {"VkFormatProperties": props}

caps = profile["capabilities"]
caps.pop("m2v_vulkan12_required_formats", None)
caps["m2v_a540_formats"] = {"formats": dict(sorted(formats.items()))}
for p in profile["profiles"].values():
    p["capabilities"] = [c if c != "m2v_vulkan12_required_formats" else "m2v_a540_formats" for c in p["capabilities"]]
profile["provenance"] = {
    "source": "tools/sweetmtl/m0/a540-gallium.json (hand-made from vk12-floor for the taimen Metal plan, M0) "
              "with its format table made by tools/sweetmtl/profile/make-profile.py from freedreno a5xx's "
              "answers for gpu 540 (gen-formats.c on drm-shim)"
}
with open(out, "w") as f:
    json.dump(profile, f, indent=2)
    f.write("\n")
print(f"{out}: {len(formats)} formats")
