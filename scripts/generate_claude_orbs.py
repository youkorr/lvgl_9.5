#!/usr/bin/env python3
"""
Generate Lottie JSON files for Claude voice-assistant orb animations.

Produces 4 variants matching the voice_assistant pipeline states:
  - claude_orb_idle.json       (slow breathing, cool green)
  - claude_orb_listening.json  (fast pulse, bright green)
  - claude_orb_thinking.json   (rotation, Claude orange)
  - claude_orb_replying.json   (wave-like, warm gold)

LIVE-REACTIVE PROPERTIES (option C — Lottie expressions)
--------------------------------------------------------

Each orb exposes 3 named layers with expression-driven properties that can be
updated at runtime via `tvg_lottie_animation_assign(anim, layer, ix, var, val)`.
The JS expressions declare the variables with a default of 0.0 so the orb
still looks correct when nothing is being assigned (e.g. during idle).

  Layer name          ix   var       Effect on the orb
  ------------------  ---  --------  ---------------------------------------
  orb_reactive_core   201  bass      Core scale (pump: 100 → 100+bass*60)
  orb_reactive_glow   202  mid       Glow opacity (breath: 60 → 60+mid*40)
  orb_reactive_bloom  203  treble    Bloom scale (shimmer: 95 → 95+treble*25)

Callers can assign values in [0.0, 1.0] (unclamped — the expression soft-caps
the final visual output). A value of 0.0 restores the "resting" look.

Build requirement: thorvg must be built with
  CONFIG_THORVG_LOTTIE_EXPRESSIONS_SUPPORT=y

Without it, the expressions are ignored and the orbs fall back to their
static keyframe values (which are tuned to look good on their own).

Output directory: ../sdcard_assets/claude/
"""

import json
import os
from copy import deepcopy

# ---------- Constants ----------

SIZE = 400  # canvas size (square)
CENTER = SIZE / 2
FPS = 60
DUR = 300  # frames (5 s at 60fps)

# Reactive property indexes — referenced from the ESPHome YAML fragment.
# Must be unique within the file. Keep in sync with README.
IX_CORE_SCALE = 201
IX_GLOW_OPACITY = 202
IX_BLOOM_SCALE = 203

# Reactive layer names (targeted by thorvg assign() layer argument).
LAYER_CORE = "orb_reactive_core"
LAYER_GLOW = "orb_reactive_glow"
LAYER_BLOOM = "orb_reactive_bloom"


# ---------- Helpers ----------

def rgb(hexstr, a=1.0):
    """Convert #RRGGBB to normalized [r,g,b,a]."""
    h = hexstr.lstrip("#")
    return [int(h[0:2], 16) / 255, int(h[2:4], 16) / 255, int(h[4:6], 16) / 255, a]


def static(value):
    return {"a": 0, "k": value}


def anim(keyframes):
    """keyframes = [(t, value), ...]. Smooth interpolation."""
    k = []
    for i, (t, v) in enumerate(keyframes):
        kf = {"t": t, "s": v if isinstance(v, list) else [v]}
        if i < len(keyframes) - 1:
            kf["i"] = {"x": [0.5] * len(kf["s"]), "y": [1.0] * len(kf["s"])}
            kf["o"] = {"x": [0.5] * len(kf["s"]), "y": [0.0] * len(kf["s"])}
        k.append(kf)
    return {"a": 1, "k": k}


def reactive_scale(ix, var_name, base=100, gain=60):
    """
    Expression-driven 3D scale property.

    Declares `var` with default 0.0 then computes [base+var*gain, base+var*gain, 100].
    Can be patched at runtime via assign(layer, ix, var_name, value).
    """
    code = (
        f"var {var_name}=0.0;"
        f"var s={base}+{var_name}*{gain};"
        f"[s,s,100];"
    )
    return {
        "a": 0,
        "k": [base, base, 100],
        "ix": ix,
        "x": code,
    }


def reactive_opacity(ix, var_name, base=60, gain=40):
    """
    Expression-driven opacity (scalar 0-100).
    """
    code = (
        f"var {var_name}=0.0;"
        f"var o={base}+{var_name}*{gain};"
        f"o;"
    )
    return {
        "a": 0,
        "k": base,
        "ix": ix,
        "x": code,
    }


def transform(pos=None, scale=100, rot=0, opacity=100, anchor=(0, 0)):
    return {
        "o": opacity if isinstance(opacity, dict) else static(opacity),
        "r": rot if isinstance(rot, dict) else static(rot),
        "p": static([pos[0], pos[1], 0]) if pos else static([CENTER, CENTER, 0]),
        "a": static([anchor[0], anchor[1], 0]),
        "s": scale if isinstance(scale, dict) else static([scale, scale, 100]),
    }


def reactive_transform(
    pos=None,
    scale_ix=None, scale_var=None, scale_base=100, scale_gain=60,
    opacity_ix=None, opacity_var=None, opacity_base=100, opacity_gain=0,
    rot=0,
):
    """Build a transform dict where scale and/or opacity are expression-driven."""
    s = (reactive_scale(scale_ix, scale_var, scale_base, scale_gain)
         if scale_ix is not None else static([scale_base, scale_base, 100]))
    o = (reactive_opacity(opacity_ix, opacity_var, opacity_base, opacity_gain)
         if opacity_ix is not None else static(opacity_base))
    return {
        "o": o,
        "r": rot if isinstance(rot, dict) else static(rot),
        "p": static([pos[0], pos[1], 0]) if pos else static([CENTER, CENTER, 0]),
        "a": static([0, 0, 0]),
        "s": s,
    }


def shape_transform(pos=(0, 0), scale=100, rot=0, opacity=100):
    return {
        "ty": "tr",
        "p": static(list(pos)),
        "a": static([0, 0]),
        "s": scale if isinstance(scale, dict) else static([scale, scale]),
        "r": rot if isinstance(rot, dict) else static(rot),
        "o": opacity if isinstance(opacity, dict) else static(opacity),
    }


def ellipse(w, h, pos=(0, 0)):
    return {"ty": "el", "d": 1, "s": static([w, h]), "p": static(list(pos))}


def fill(color):
    return {"ty": "fl", "c": static(color[:3] + [1]), "o": static(color[3] * 100), "r": 1}


def stroke(color, width=1.5):
    return {
        "ty": "st",
        "c": static(color[:3] + [1]),
        "o": static(color[3] * 100),
        "w": static(width),
        "lc": 2,
        "lj": 2,
    }


def group(items, name="group"):
    return {"ty": "gr", "it": items, "nm": name}


def shape_layer(ind, name, shapes, tr):
    return {
        "ddd": 0,
        "ind": ind,
        "ty": 4,
        "nm": name,
        "sr": 1,
        "ks": tr,
        "ao": 0,
        "shapes": shapes,
        "ip": 0,
        "op": DUR,
        "st": 0,
        "bm": 0,
    }


def filled_circle_layer(ind, name, diameter, color, opacity_anim=None, scale_anim=None):
    shapes = [group([ellipse(diameter, diameter), fill(color), shape_transform()], name)]
    tr = transform(opacity=opacity_anim or 100, scale=scale_anim or 100)
    return shape_layer(ind, name, shapes, tr)


def reactive_circle_layer(ind, name, diameter, color,
                          scale_ix=None, scale_var=None, scale_base=100, scale_gain=60,
                          opacity_ix=None, opacity_var=None, opacity_base=100, opacity_gain=0):
    """Filled circle layer with expression-driven scale and/or opacity."""
    shapes = [group([ellipse(diameter, diameter), fill(color), shape_transform()], name)]
    tr = reactive_transform(
        scale_ix=scale_ix, scale_var=scale_var,
        scale_base=scale_base, scale_gain=scale_gain,
        opacity_ix=opacity_ix, opacity_var=opacity_var,
        opacity_base=opacity_base, opacity_gain=opacity_gain,
    )
    return shape_layer(ind, name, shapes, tr)


def wireframe_ring(ind, name, w, h, color, stroke_w, rot_anim, opacity_val=70):
    shapes = [
        group(
            [ellipse(w, h), stroke(color, stroke_w), shape_transform()],
            name,
        )
    ]
    tr = transform(rot=rot_anim, opacity=opacity_val)
    return shape_layer(ind, name, shapes, tr)


def hotspot_layer(ind, name, pos, diameter, color, blink_anim):
    shapes = [
        group(
            [ellipse(diameter, diameter), fill(color), shape_transform()],
            name,
        )
    ]
    tr = transform(pos=pos, opacity=blink_anim)
    return shape_layer(ind, name, shapes, tr)


# ---------- Animation curves ----------

def rotate_cw(duration=DUR):
    return anim([(0, 0), (duration, 360)])


def rotate_ccw(duration=DUR):
    return anim([(0, 0), (duration, -360)])


def breathe(min_s=95, max_s=105, period=DUR):
    half = period // 2
    return anim([(0, [min_s, min_s, 100]),
                 (half, [max_s, max_s, 100]),
                 (period, [min_s, min_s, 100])])


def pulse_opacity(min_o=40, max_o=100, period=60):
    """Sharp pulse for listening state."""
    half = period // 2
    kfs = []
    for i in range(DUR // period + 1):
        t0 = i * period
        kfs.append((t0, min_o))
        kfs.append((t0 + half, max_o))
    kfs.append((DUR, min_o))
    return anim(kfs)


def blink(min_o=0, max_o=100, period=90, phase=0):
    kfs = []
    for i in range(DUR // period + 2):
        t0 = i * period + phase
        if t0 > DUR:
            break
        kfs.append((max(0, t0), min_o))
        kfs.append((min(DUR, t0 + period // 2), max_o))
    kfs.append((DUR, min_o))
    # dedupe & sort
    seen = set()
    out = []
    for t, v in sorted(kfs):
        if t not in seen:
            seen.add(t)
            out.append((t, v))
    return anim(out)


# ---------- Orb builder ----------

def build_orb(name, palette, rot_period=300, breath_period=180, pulse_period=60):
    """
    palette = {
      'bloom': '#RRGGBB', 'glow': ..., 'core': ...,
      'ring': ..., 'hot1': ..., 'hot2': ...
    }
    """
    # Saturate colors - we want luminous plasma look
    bloom = rgb(palette["bloom"], 0.35)
    glow = rgb(palette["glow"], 0.45)
    core = rgb(palette["core"], 0.80)
    ring = rgb(palette["ring"], 1.00)
    ring_bright = rgb(palette["hot2"], 1.00)
    hot1 = rgb(palette["hot1"], 1.00)
    hot2 = rgb(palette["hot2"], 1.00)

    layers = []
    ind_counter = [100]

    def next_ind():
        ind_counter[0] -= 1
        return ind_counter[0]

    # --- REACTIVE: Background bloom (treble-driven scale shimmer) ---
    # Expression: scale = 95 + treble*25 (base shimmer at 95%, grows on highs)
    layers.append(reactive_circle_layer(
        next_ind(), LAYER_BLOOM, 380, bloom,
        scale_ix=IX_BLOOM_SCALE, scale_var="treble", scale_base=95, scale_gain=25,
        opacity_base=75,
    ))

    # --- REACTIVE: Mid glow (mid-driven opacity breath) ---
    # Expression: opacity = 60 + mid*40 (breath on speech mid-band)
    layers.append(reactive_circle_layer(
        next_ind(), LAYER_GLOW, 300, glow,
        scale_base=100,
        opacity_ix=IX_GLOW_OPACITY, opacity_var="mid", opacity_base=60, opacity_gain=40,
    ))

    # --- REACTIVE: Inner core (bass-driven scale pump) ---
    # Expression: scale = 100 + bass*60 (pumps hard on low-frequency content)
    layers.append(reactive_circle_layer(
        next_ind(), LAYER_CORE, 180, core,
        scale_ix=IX_CORE_SCALE, scale_var="bass", scale_base=100, scale_gain=60,
        opacity_base=90,
    ))

    # --- Wireframe: latitudes (horizontal ellipses) ---
    # 7 latitudes at different heights create the "sphere slicing" effect
    latitudes = [
        (340, 20,  2.0),
        (340, 70,  1.8),
        (340, 130, 1.6),
        (340, 190, 1.5),
        (340, 250, 1.5),
        (340, 310, 1.5),
        (340, 340, 2.0),  # full equator outline
    ]
    for i, (w, h, sw) in enumerate(latitudes):
        col = ring_bright if i in (0, 3, 6) else ring
        op = 85 if i == 3 else 70
        rot_a = rotate_cw(rot_period) if i % 2 == 0 else rotate_ccw(int(rot_period * 1.2))
        rot_a = deepcopy(rot_a)
        if rot_a["k"][-1]["t"] > DUR:
            sf = DUR / rot_a["k"][-1]["t"]
            for kf in rot_a["k"]:
                kf["t"] = int(kf["t"] * sf)
        layers.append(wireframe_ring(next_ind(), f"lat_{i}", w, h, col, sw, rot_a, op))

    # --- Wireframe: longitudes (vertical ellipses = sphere slices) ---
    longitudes = [
        (20,  340, 2.0),
        (70,  340, 1.8),
        (130, 340, 1.6),
        (190, 340, 1.5),
        (250, 340, 1.5),
        (310, 340, 1.5),
    ]
    for i, (w, h, sw) in enumerate(longitudes):
        col = ring_bright if i in (0, 2) else ring
        op = 85 if i == 2 else 70
        rot_a = rotate_ccw(int(rot_period * 1.5)) if i % 2 == 0 else rotate_cw(int(rot_period * 1.8))
        rot_a = deepcopy(rot_a)
        if rot_a["k"][-1]["t"] > DUR:
            sf = DUR / rot_a["k"][-1]["t"]
            for kf in rot_a["k"]:
                kf["t"] = int(kf["t"] * sf)
        layers.append(wireframe_ring(next_ind(), f"lon_{i}", w, h, col, sw, rot_a, op))

    # --- Tilted wireframes (diagonal slices) ---
    tilted = [
        (320, 180, 1.4, 30),
        (300, 160, 1.4, -30),
        (280, 140, 1.3, 60),
        (260, 120, 1.3, -60),
    ]
    for i, (w, h, sw, base_rot) in enumerate(tilted):
        rot_a = anim([(0, base_rot), (DUR, base_rot + (360 if i % 2 == 0 else -360))])
        layers.append(wireframe_ring(next_ind(), f"tilt_{i}", w, h, ring, sw, rot_a, 60))

    # --- Hot spots (bright plasma points pulsing) ---
    hotspots_cfg = [
        ((CENTER + 50, CENTER + 30),  14, hot1, 90,  0),
        ((CENTER - 70, CENTER - 20),  12, hot2, 110, 40),
        ((CENTER + 80, CENTER - 60),  10, hot1, 75,  20),
        ((CENTER - 40, CENTER + 80),  10, hot2, 100, 60),
    ]
    for i, (pos, d, col, period, phase) in enumerate(hotspots_cfg):
        layers.append(hotspot_layer(
            next_ind(), f"hotspot_{i}", pos, d, col,
            blink_anim=blink(20, 100, period, phase),
        ))

    return {
        "v": "5.7.4",
        "fr": FPS,
        "ip": 0,
        "op": DUR,
        "w": SIZE,
        "h": SIZE,
        "nm": name,
        "ddd": 0,
        "assets": [],
        "layers": sorted(layers, key=lambda x: x["ind"]),
        "markers": [],
    }


# ---------- Variants ----------

PALETTES = {
    "idle": {
        "bloom": "#3A7FA0", "glow": "#4D9FC4", "core": "#B5E3F0",
        "ring": "#8FD8ED", "hot1": "#E0F5FF", "hot2": "#B5E3F0",
    },
    "listening": {
        # TikTok ref: green organic plasma
        "bloom": "#2AFF5B", "glow": "#5BFF6B", "core": "#B6FFB0",
        "ring": "#8BFF90", "hot1": "#FFFF80", "hot2": "#D9FFD0",
    },
    "thinking": {
        # Claude orange
        "bloom": "#D97757", "glow": "#E89578", "core": "#FFD6C2",
        "ring": "#F5B094", "hot1": "#FFE8D4", "hot2": "#FFC49E",
    },
    "replying": {
        # Warm gold
        "bloom": "#E8B84A", "glow": "#F5CE6B", "core": "#FFECB0",
        "ring": "#F5D87A", "hot1": "#FFF5D0", "hot2": "#FFE894",
    },
}

# Different timings per state
TIMINGS = {
    "idle":      dict(rot_period=900, breath_period=240, pulse_period=180),
    "listening": dict(rot_period=420, breath_period=120, pulse_period=45),
    "thinking":  dict(rot_period=180, breath_period=150, pulse_period=60),
    "replying":  dict(rot_period=360, breath_period=90,  pulse_period=40),
}


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "sdcard_assets", "claude")
    out_dir = os.path.normpath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    for state, palette in PALETTES.items():
        data = build_orb(
            name=f"ClaudeOrb_{state}",
            palette=palette,
            **TIMINGS[state],
        )
        path = os.path.join(out_dir, f"claude_orb_{state}.json")
        with open(path, "w") as f:
            json.dump(data, f, separators=(",", ":"))
        size_kb = os.path.getsize(path) / 1024
        print(f"  wrote {path}  ({size_kb:.1f} KB)")

    print("done.")
    print()
    print("Reactive binding map (use with tvg_lottie_animation_assign):")
    print(f"  layer={LAYER_CORE!r:30s} ix={IX_CORE_SCALE}  var='bass'   -> core scale")
    print(f"  layer={LAYER_GLOW!r:30s} ix={IX_GLOW_OPACITY}  var='mid'    -> glow opacity")
    print(f"  layer={LAYER_BLOOM!r:30s} ix={IX_BLOOM_SCALE}  var='treble' -> bloom scale")


if __name__ == "__main__":
    main()
