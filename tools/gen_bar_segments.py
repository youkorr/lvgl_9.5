#!/usr/bin/env python3
"""
Generate the two bitmaps for an LVGL "image indicator" segmented bar
(à la lv_example_bar_img_indicator), using the user's own palette.

Outputs:
  - bar_track.png      -> LV_PART_MAIN      (segments "off" = couleur assombrie)
  - bar_indicator.png  -> LV_PART_INDICATOR (segments "on"  = couleur pleine)

Les deux images partagent EXACTEMENT la même géométrie : quand l'indicateur
se remplit de gauche à droite, il révèle les segments allumés par-dessus les
segments éteints, sans décalage.
"""
from PIL import Image, ImageDraw

# ---- Taille (doit = largeur de contenu du widget : width - pad_left - pad_right) ----
W, H = 280, 40

# ---- Disposition des segments ----
N_CYAN, N_YELLOW, N_RED = 13, 1, 2          # 16 segments au total
MARGIN = 3                                   # marge intérieure (px)
GAP = 4                                       # espace noir entre segments (px)
RADIUS = 3                                    # arrondi des coins (px)
OUTLINE_W = 1                                 # liseré sombre autour de chaque segment

# ---- Couleurs (TA palette) ----
BG          = (0x0d, 0x0d, 0x0d)             # color_black  -> fond + séparateurs
CYAN        = (0x00, 0xFF, 0xFF)             # color_cyan
YELLOW      = (0xe7, 0xc1, 0x2c)             # color_yellow
RED         = (0xf5, 0x07, 0x5c)             # color_crimson

OFF_FACTOR  = 0.20                            # luminosité des segments éteints (piste)


def darken(rgb, f):
    return tuple(int(c * f) for c in rgb)


def outline_of(rgb):
    return darken(rgb, 0.55)


def seg_colors():
    return [CYAN] * N_CYAN + [YELLOW] * N_YELLOW + [RED] * N_RED


def render(on: bool):
    colors = seg_colors()
    n = len(colors)
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    usable = W - 2 * MARGIN - (n - 1) * GAP
    seg_w = usable / n
    y1, y2 = MARGIN, H - 1 - MARGIN

    x = MARGIN
    for c in colors:
        fill = c if on else darken(c, OFF_FACTOR)
        line = outline_of(c) if on else darken(c, OFF_FACTOR * 0.6)
        x1 = round(x)
        x2 = round(x + seg_w) - 1
        d.rounded_rectangle(
            [x1, y1, x2, y2],
            radius=RADIUS,
            fill=fill,
            outline=line,
            width=OUTLINE_W,
        )
        x += seg_w + GAP
    return img


if __name__ == "__main__":
    import os
    out = os.path.dirname(os.path.abspath(__file__))
    render(on=False).save(os.path.join(out, "bar_track.png"))
    render(on=True).save(os.path.join(out, "bar_indicator.png"))
    print(f"OK -> bar_track.png + bar_indicator.png  ({W}x{H}, "
          f"{N_CYAN}+{N_YELLOW}+{N_RED}={N_CYAN+N_YELLOW+N_RED} segments)")
