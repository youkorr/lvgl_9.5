#!/usr/bin/env python3
"""
Generate the two bitmaps for an LVGL "image indicator" segmented bar
(à la lv_example_bar_img_indicator), using the user's own palette.

Outputs (<prefix> défaut "bar") :
  - <prefix>_track.png      -> LV_PART_MAIN      (segments "off" = couleur assombrie)
  - <prefix>_indicator.png  -> LV_PART_INDICATOR (segments "on"  = couleur pleine)

Les deux images partagent EXACTEMENT la même géométrie : quand l'indicateur
se remplit de gauche à droite, il révèle les segments allumés par-dessus les
segments éteints, sans décalage.

Exemples :
  python3 gen_bar_segments.py                              # bar_* 280x40
  python3 gen_bar_segments.py --width 260 --height 60 \
                              --prefix slider               # slider_* 260x60
"""
import argparse
import os

from PIL import Image, ImageDraw

# ---- Couleurs (palette utilisateur) ----
BG     = (0x0d, 0x0d, 0x0d)   # color_black  -> fond + séparateurs
CYAN   = (0x00, 0xFF, 0xFF)   # color_cyan
YELLOW = (0xe7, 0xc1, 0x2c)   # color_yellow
RED    = (0xf5, 0x07, 0x5c)   # color_crimson


def darken(rgb, f):
    return tuple(int(c * f) for c in rgb)


def render(on, *, w, h, n_cyan, n_yellow, n_red,
           margin, gap, radius, outline_w, off_factor):
    colors = [CYAN] * n_cyan + [YELLOW] * n_yellow + [RED] * n_red
    n = len(colors)
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)

    usable = w - 2 * margin - (n - 1) * gap
    seg_w = usable / n
    y1, y2 = margin, h - 1 - margin

    x = margin
    for c in colors:
        fill = c if on else darken(c, off_factor)
        line = darken(c, 0.55) if on else darken(c, off_factor * 0.6)
        x1 = round(x)
        x2 = round(x + seg_w) - 1
        d.rounded_rectangle([x1, y1, x2, y2], radius=radius,
                            fill=fill, outline=line, width=outline_w)
        x += seg_w + gap
    return img


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--width", type=int, default=280)
    p.add_argument("--height", type=int, default=40)
    p.add_argument("--prefix", default="bar", help="préfixe des fichiers de sortie")
    p.add_argument("--cyan", type=int, default=13, help="nb de segments cyan")
    p.add_argument("--yellow", type=int, default=1, help="nb de segments jaunes")
    p.add_argument("--red", type=int, default=2, help="nb de segments rouges")
    p.add_argument("--margin", type=int, default=3)
    p.add_argument("--gap", type=int, default=4, help="espace entre segments (px)")
    p.add_argument("--radius", type=int, default=3, help="arrondi des coins (px)")
    p.add_argument("--outline", type=int, default=1, help="liseré par segment (px)")
    p.add_argument("--off", type=float, default=0.20,
                   help="luminosité des segments éteints (0..1)")
    p.add_argument("--outdir", default=os.path.dirname(os.path.abspath(__file__)))
    a = p.parse_args()

    kw = dict(w=a.width, h=a.height, n_cyan=a.cyan, n_yellow=a.yellow, n_red=a.red,
              margin=a.margin, gap=a.gap, radius=a.radius, outline_w=a.outline,
              off_factor=a.off)

    track = os.path.join(a.outdir, f"{a.prefix}_track.png")
    indic = os.path.join(a.outdir, f"{a.prefix}_indicator.png")
    render(False, **kw).save(track)
    render(True, **kw).save(indic)
    print(f"OK -> {os.path.basename(track)} + {os.path.basename(indic)} "
          f"({a.width}x{a.height}, {a.cyan}+{a.yellow}+{a.red}="
          f"{a.cyan + a.yellow + a.red} segments)")


if __name__ == "__main__":
    main()
