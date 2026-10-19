#!/usr/bin/env python3
import argparse
import glob
import os
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

FACES = [(9, 20), (11, 24), (14, 31)]
ADVANCE_EM = 0.6
ASCENT_EM = 1.02
DESCENT_EM = 0.3
GAMMA = 0.82
FALLBACK_CP = 0xFFFD
SUBSETS = ["latin", "latin-ext", "greek", "cyrillic", "cyrillic-ext", "vietnamese"]


def load_fonts(font_dir, weight):
    fonts = []
    for subset in SUBSETS:
        path = os.path.join(font_dir, "jetbrains-mono-%s-%d-normal.woff" % (subset, weight))
        if not os.path.exists(path):
            sys.exit("missing font file: " + path)
        cmap = set(TTFont(path).getBestCmap().keys())
        fonts.append((path, cmap))
    return fonts


def metrics(w, h):
    em = w / ADVANCE_EM
    asc = ASCENT_EM * em
    desc = DESCENT_EM * em
    baseline = int(round(asc + (h - (asc + desc)) / 2.0))
    return em, baseline


def quantize(img):
    out = []
    px = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            a = px[x, y] / 255.0
            a = a ** GAMMA
            out.append(min(15, int(a * 15 + 0.5)))
    return out


def render_font_glyph(cp, font, w, h, baseline):
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    d.text((0, baseline), chr(cp), font=font, fill=255, anchor="ls")
    return img


def thick(w):
    tl = max(1, int(round(w / 9.0)))
    th = 2 * tl - (1 if tl >= 2 else 0)
    return tl, th


def rect(px, w, h, x0, y0, x1, y1, v=255):
    for y in range(max(0, y0), min(h - 1, y1) + 1):
        for x in range(max(0, x0), min(w - 1, x1) + 1):
            px[x, y] = v


BOX = {}


def _b(cp, l, r, u, d):
    BOX[cp] = (l, r, u, d)


def _fill_box_table():
    rows = [
        (0x2500, 1, 1, 0, 0), (0x2501, 2, 2, 0, 0), (0x2502, 0, 0, 1, 1), (0x2503, 0, 0, 2, 2),
        (0x250C, 0, 1, 0, 1), (0x250D, 0, 2, 0, 1), (0x250E, 0, 1, 0, 2), (0x250F, 0, 2, 0, 2),
        (0x2510, 1, 0, 0, 1), (0x2511, 2, 0, 0, 1), (0x2512, 1, 0, 0, 2), (0x2513, 2, 0, 0, 2),
        (0x2514, 0, 1, 1, 0), (0x2515, 0, 2, 1, 0), (0x2516, 0, 1, 2, 0), (0x2517, 0, 2, 2, 0),
        (0x2518, 1, 0, 1, 0), (0x2519, 2, 0, 1, 0), (0x251A, 1, 0, 2, 0), (0x251B, 2, 0, 2, 0),
        (0x251C, 0, 1, 1, 1), (0x251D, 0, 2, 1, 1), (0x251E, 0, 1, 2, 1), (0x251F, 0, 1, 1, 2),
        (0x2520, 0, 1, 2, 2), (0x2521, 0, 2, 2, 1), (0x2522, 0, 2, 1, 2), (0x2523, 0, 2, 2, 2),
        (0x2524, 1, 0, 1, 1), (0x2525, 2, 0, 1, 1), (0x2526, 1, 0, 2, 1), (0x2527, 1, 0, 1, 2),
        (0x2528, 1, 0, 2, 2), (0x2529, 2, 0, 2, 1), (0x252A, 2, 0, 1, 2), (0x252B, 2, 0, 2, 2),
        (0x252C, 1, 1, 0, 1), (0x252D, 2, 1, 0, 1), (0x252E, 1, 2, 0, 1), (0x252F, 2, 2, 0, 1),
        (0x2530, 1, 1, 0, 2), (0x2531, 2, 1, 0, 2), (0x2532, 1, 2, 0, 2), (0x2533, 2, 2, 0, 2),
        (0x2534, 1, 1, 1, 0), (0x2535, 2, 1, 1, 0), (0x2536, 1, 2, 1, 0), (0x2537, 2, 2, 1, 0),
        (0x2538, 1, 1, 2, 0), (0x2539, 2, 1, 2, 0), (0x253A, 1, 2, 2, 0), (0x253B, 2, 2, 2, 0),
        (0x253C, 1, 1, 1, 1), (0x253D, 2, 1, 1, 1), (0x253E, 1, 2, 1, 1), (0x253F, 2, 2, 1, 1),
        (0x2540, 1, 1, 2, 1), (0x2541, 1, 1, 1, 2), (0x2542, 1, 1, 2, 2), (0x2543, 2, 1, 2, 1),
        (0x2544, 1, 2, 2, 1), (0x2545, 2, 1, 1, 2), (0x2546, 1, 2, 1, 2), (0x2547, 2, 2, 2, 1),
        (0x2548, 2, 2, 1, 2), (0x2549, 2, 1, 2, 2), (0x254A, 1, 2, 2, 2), (0x254B, 2, 2, 2, 2),
        (0x2550, 3, 3, 0, 0), (0x2551, 0, 0, 3, 3), (0x2552, 0, 3, 0, 1), (0x2553, 0, 1, 0, 3),
        (0x2554, 0, 3, 0, 3), (0x2555, 3, 0, 0, 1), (0x2556, 1, 0, 0, 3), (0x2557, 3, 0, 0, 3),
        (0x2558, 0, 3, 1, 0), (0x2559, 0, 1, 3, 0), (0x255A, 0, 3, 3, 0), (0x255B, 3, 0, 1, 0),
        (0x255C, 1, 0, 3, 0), (0x255D, 3, 0, 3, 0), (0x255E, 0, 3, 1, 1), (0x255F, 0, 1, 3, 3),
        (0x2560, 0, 3, 3, 3), (0x2561, 3, 0, 1, 1), (0x2562, 1, 0, 3, 3), (0x2563, 3, 0, 3, 3),
        (0x2564, 3, 3, 0, 1), (0x2565, 1, 1, 0, 3), (0x2566, 3, 3, 0, 3), (0x2567, 3, 3, 1, 0),
        (0x2568, 1, 1, 3, 0), (0x2569, 3, 3, 3, 0), (0x256A, 3, 3, 1, 1), (0x256B, 1, 1, 3, 3),
        (0x256C, 3, 3, 3, 3), (0x2574, 1, 0, 0, 0), (0x2575, 0, 0, 1, 0), (0x2576, 0, 1, 0, 0),
        (0x2577, 0, 0, 0, 1), (0x2578, 2, 0, 0, 0), (0x2579, 0, 0, 2, 0), (0x257A, 0, 2, 0, 0),
        (0x257B, 0, 0, 0, 2), (0x257C, 1, 2, 0, 0), (0x257D, 0, 0, 1, 2), (0x257E, 2, 1, 0, 0),
        (0x257F, 0, 0, 2, 1),
    ]
    for r in rows:
        _b(*r)


_fill_box_table()

DASHED = {
    0x2504: ("h", 3, 1), 0x2505: ("h", 3, 2), 0x2506: ("v", 3, 1), 0x2507: ("v", 3, 2),
    0x2508: ("h", 4, 1), 0x2509: ("h", 4, 2), 0x250A: ("v", 4, 1), 0x250B: ("v", 4, 2),
    0x254C: ("h", 2, 1), 0x254D: ("h", 2, 2), 0x254E: ("v", 2, 1), 0x254F: ("v", 2, 2),
}

ARCS = {0x256D: "dr", 0x256E: "dl", 0x256F: "ul", 0x2570: "ur"}


def stroke_range(c0, t):
    s = c0 - (t - 1) // 2
    return s, s + t - 1


def double_ranges(c0, t):
    g = max(2, t + 1)
    total = 2 * t + g
    a0 = c0 - (total - 1) // 2
    a1 = a0 + t - 1
    b0 = a0 + t + g
    b1 = b0 + t - 1
    return (a0, a1), (b0, b1)


def draw_box(cp, w, h):
    img = Image.new("L", (w, h), 0)
    px = img.load()
    tl, th = thick(w)
    tlv, thv = thick(w)
    if h >= 28:
        tlv = max(tlv, 2)
        thv = 2 * tlv - 1
    thick_h = {1: tl, 2: th}
    thick_v = {1: tlv, 2: thv}
    cx = (w - 1) // 2
    cy = (h - 1) // 2

    if cp in DASHED:
        axis, n, wt = DASHED[cp]
        if axis == "h":
            t = thick_h[wt]
            y0, y1 = stroke_range(cy, t)
            seg = w / float(n)
            for i in range(n):
                x0 = int(round(i * seg + seg * 0.2))
                x1 = int(round(i * seg + seg * 0.8)) - 1
                rect(px, w, h, x0, y0, max(x0, x1), y1)
        else:
            t = thick_v[wt]
            x0, x1 = stroke_range(cx, t)
            seg = h / float(n)
            for i in range(n):
                y0 = int(round(i * seg + seg * 0.2))
                y1 = int(round(i * seg + seg * 0.8)) - 1
                rect(px, w, h, x0, y0, x1, max(y0, y1))
        return img

    if cp in ARCS:
        return draw_arc(ARCS[cp], w, h, tl)

    if cp in (0x2571, 0x2572, 0x2573):
        s = 4
        big = Image.new("L", (w * s, h * s), 0)
        d = ImageDraw.Draw(big)
        lw = max(1, int(round(tl * s * 0.9)))
        if cp in (0x2571, 0x2573):
            d.line([(w * s, 0), (0, h * s)], fill=255, width=lw)
        if cp in (0x2572, 0x2573):
            d.line([(0, 0), (w * s, h * s)], fill=255, width=lw)
        return big.resize((w, h), Image.BOX)

    l, r, u, d = BOX[cp]
    axis_h_double = (l == 3 or r == 3)
    axis_v_double = (u == 3 or d == 3)

    def hthick(s):
        return thick_h[s] if s in thick_h else 0

    def vthick(s):
        return thick_v[s] if s in thick_v else 0

    if axis_v_double:
        (va0, va1), (vb0, vb1) = double_ranges(cx, tlv)
        vj0, vj1 = va0, vb1
    else:
        vj_t = max(vthick(u), vthick(d)) if (u or d) else 1
        vj0, vj1 = stroke_range(cx, vj_t)
        va0 = va1 = vb0 = vb1 = None
    if axis_h_double:
        (ha0, ha1), (hb0, hb1) = double_ranges(cy, tl)
        hj0, hj1 = ha0, hb1
    else:
        hj_t = max(hthick(l), hthick(r)) if (l or r) else 1
        hj0, hj1 = stroke_range(cy, hj_t)
        ha0 = ha1 = hb0 = hb1 = None

    if axis_h_double:
        for (s0, s1), side in (((ha0, ha1), u), ((hb0, hb1), d)):
            other = d if side is u else u
            blocked = side == 3
            other_double = other == 3
            left_end = right_start = None
            if l:
                if blocked:
                    left_end = va1
                elif other_double:
                    left_end = vb1
                elif u == 3 or d == 3:
                    left_end = vj1
                else:
                    left_end = cx
            if r:
                if blocked:
                    right_start = vb0
                elif other_double:
                    right_start = va0
                elif u == 3 or d == 3:
                    right_start = vj0
                else:
                    right_start = cx
            if l and r and not blocked and not other_double:
                rect(px, w, h, 0, s0, w - 1, s1)
            else:
                if l and left_end is not None:
                    rect(px, w, h, 0, s0, left_end, s1)
                if r and right_start is not None:
                    rect(px, w, h, right_start, s0, w - 1, s1)
    else:
        for arm, start_x, end_x in ((l, 0, None), (r, None, w - 1)):
            if not arm:
                continue
            t = hthick(arm)
            y0, y1 = stroke_range(cy, t)
            if start_x == 0:
                if r and not axis_v_double:
                    rect(px, w, h, 0, y0, w - 1, y1) if (r == l) else rect(px, w, h, 0, y0, vj1 if (u or d) else cx, y1)
                else:
                    ex = vj1 if (u or d) else cx
                    rect(px, w, h, 0, y0, ex, y1)
            else:
                sx = vj0 if (u or d) else cx
                if l and l == r and not axis_v_double:
                    pass
                else:
                    rect(px, w, h, sx, y0, w - 1, y1)

    if axis_v_double:
        for (s0, s1), side in (((va0, va1), l), ((vb0, vb1), r)):
            other = r if side is l else l
            blocked = side == 3
            other_double = other == 3
            top_end = bot_start = None
            if u:
                if blocked:
                    top_end = ha1
                elif other_double:
                    top_end = hb1
                elif l == 3 or r == 3:
                    top_end = hj1
                else:
                    top_end = cy
            if d:
                if blocked:
                    bot_start = hb0
                elif other_double:
                    bot_start = ha0
                elif l == 3 or r == 3:
                    bot_start = hj0
                else:
                    bot_start = cy
            if u and d and not blocked and not other_double:
                rect(px, w, h, s0, 0, s1, h - 1)
            else:
                if u and top_end is not None:
                    rect(px, w, h, s0, 0, s1, top_end)
                if d and bot_start is not None:
                    rect(px, w, h, s0, bot_start, s1, h - 1)
    else:
        for arm, up in ((u, True), (d, False)):
            if not arm:
                continue
            t = vthick(arm)
            x0, x1 = stroke_range(cx, t)
            if up:
                ey = hj1 if (l or r) else cy
                rect(px, w, h, x0, 0, x1, ey)
            else:
                sy = hj0 if (l or r) else cy
                rect(px, w, h, x0, sy, x1, h - 1)
        if u and d and u == d:
            t = vthick(u)
            x0, x1 = stroke_range(cx, t)
            rect(px, w, h, x0, 0, x1, h - 1)
    if not axis_h_double and l and r and l == r:
        t = hthick(l)
        y0, y1 = stroke_range(cy, t)
        rect(px, w, h, 0, y0, w - 1, y1)
    return img


def draw_arc(kind, w, h, tl):
    s = 8
    big = Image.new("L", (w * s, h * s), 0)
    d = ImageDraw.Draw(big)
    lw = max(1, int(round(tl * s)))
    cx = ((w - 1) // 2 + 0.5) * s
    cy = ((h - 1) // 2 + 0.5) * s
    rad = min(w, h) * s * 0.42
    rad = min(rad, w * s * 0.5 - 1)
    if kind == "dr":
        d.arc([cx, cy, cx + 2 * rad, cy + 2 * rad], 180, 270, fill=255, width=lw)
        d.line([(cx + rad, cy), (w * s, cy)], fill=255, width=lw)
        d.line([(cx, cy + rad), (cx, h * s)], fill=255, width=lw)
    elif kind == "dl":
        d.arc([cx - 2 * rad, cy, cx, cy + 2 * rad], 270, 360, fill=255, width=lw)
        d.line([(0, cy), (cx - rad, cy)], fill=255, width=lw)
        d.line([(cx, cy + rad), (cx, h * s)], fill=255, width=lw)
    elif kind == "ul":
        d.arc([cx - 2 * rad, cy - 2 * rad, cx, cy], 0, 90, fill=255, width=lw)
        d.line([(0, cy), (cx - rad, cy)], fill=255, width=lw)
        d.line([(cx, 0), (cx, cy - rad)], fill=255, width=lw)
    else:
        d.arc([cx, cy - 2 * rad, cx + 2 * rad, cy], 90, 180, fill=255, width=lw)
        d.line([(cx + rad, cy), (w * s, cy)], fill=255, width=lw)
        d.line([(cx, 0), (cx, cy - rad)], fill=255, width=lw)
    return big.resize((w, h), Image.BOX)


def draw_block(cp, w, h):
    img = Image.new("L", (w, h), 0)
    px = img.load()

    def frac_x(n, d):
        return int(round(w * n / d))

    def frac_y(n, d):
        return int(round(h * n / d))

    if cp == 0x2580:
        rect(px, w, h, 0, 0, w - 1, frac_y(1, 2) - 1)
    elif 0x2581 <= cp <= 0x2588:
        n = cp - 0x2580
        rect(px, w, h, 0, h - frac_y(n, 8), w - 1, h - 1)
    elif 0x2589 <= cp <= 0x258F:
        n = 8 - (cp - 0x2588)
        rect(px, w, h, 0, 0, frac_x(n, 8) - 1, h - 1)
    elif cp == 0x2590:
        rect(px, w, h, frac_x(1, 2), 0, w - 1, h - 1)
    elif cp in (0x2591, 0x2592, 0x2593):
        level = {0x2591: 64, 0x2592: 128, 0x2593: 192}[cp]
        rect(px, w, h, 0, 0, w - 1, h - 1, level)
    elif cp == 0x2594:
        rect(px, w, h, 0, 0, w - 1, max(0, frac_y(1, 8) - 1))
    elif cp == 0x2595:
        rect(px, w, h, w - max(1, frac_x(1, 8)), 0, w - 1, h - 1)
    else:
        quads = {
            0x2596: (0, 0, 1, 0), 0x2597: (0, 0, 0, 1), 0x2598: (1, 0, 0, 0),
            0x2599: (1, 0, 1, 1), 0x259A: (1, 0, 0, 1), 0x259B: (1, 1, 1, 0),
            0x259C: (1, 1, 0, 1), 0x259D: (0, 1, 0, 0), 0x259E: (0, 1, 1, 0),
            0x259F: (0, 1, 1, 1),
        }
        ul, ur, ll, lr = quads[cp]
        mx, my = frac_x(1, 2), frac_y(1, 2)
        if ul:
            rect(px, w, h, 0, 0, mx - 1, my - 1)
        if ur:
            rect(px, w, h, mx, 0, w - 1, my - 1)
        if ll:
            rect(px, w, h, 0, my, mx - 1, h - 1)
        if lr:
            rect(px, w, h, mx, my, w - 1, h - 1)
    return img


def draw_braille(cp, w, h):
    s = 6
    big = Image.new("L", (w * s, h * s), 0)
    d = ImageDraw.Draw(big)
    bits = cp - 0x2800
    cols = (0.30, 0.70)
    rows = (0.16, 0.38, 0.60, 0.82)
    order = [(0, 0, 0x01), (0, 1, 0x02), (0, 2, 0x04), (1, 0, 0x08), (1, 1, 0x10), (1, 2, 0x20), (0, 3, 0x40), (1, 3, 0x80)]
    rad = w * s * 0.115
    for c, r, mask in order:
        if bits & mask:
            x = cols[c] * w * s
            y = rows[r] * h * s
            d.ellipse([x - rad, y - rad, x + rad, y + rad], fill=255)
    return big.resize((w, h), Image.BOX)


def poly(w, h, pts, s=8):
    big = Image.new("L", (w * s, h * s), 0)
    d = ImageDraw.Draw(big)
    d.polygon([(x * w * s, y * h * s) for x, y in pts], fill=255)
    return big.resize((w, h), Image.BOX)


def draw_shape(cp, w, h, baseline):
    s = 8
    big = Image.new("L", (w * s, h * s), 0)
    d = ImageDraw.Draw(big)
    cxs = w * s / 2.0
    cys = (baseline - h * 0.22) * s
    r = w * s * 0.40
    lw = max(2, int(round(w * s * 0.11)))
    if cp in (0x25CF, 0x25CB, 0x25E6, 0x2022):
        rr = r * (0.55 if cp == 0x25E6 else 1.0)
        if cp == 0x25CB:
            d.ellipse([cxs - rr, cys - rr, cxs + rr, cys + rr], outline=255, width=lw)
        else:
            d.ellipse([cxs - rr, cys - rr, cxs + rr, cys + rr], fill=255)
    elif cp in (0x25A0, 0x25A1, 0x25AA, 0x25AB):
        rr = r * (0.7 if cp in (0x25AA, 0x25AB) else 0.95)
        box = [cxs - rr, cys - rr, cxs + rr, cys + rr]
        if cp in (0x25A1, 0x25AB):
            d.rectangle(box, outline=255, width=lw)
        else:
            d.rectangle(box, fill=255)
    elif cp in (0x25B2, 0x25BC, 0x25B6, 0x25C0, 0x25B8, 0x25BE, 0x25C2, 0x25B4):
        k = 0.95 if cp in (0x25B2, 0x25BC, 0x25B6, 0x25C0) else 0.7
        q = r * k
        if cp in (0x25B2, 0x25B4):
            pts = [(cxs, cys - q), (cxs + q, cys + q * 0.8), (cxs - q, cys + q * 0.8)]
        elif cp in (0x25BC, 0x25BE):
            pts = [(cxs, cys + q), (cxs + q, cys - q * 0.8), (cxs - q, cys - q * 0.8)]
        elif cp in (0x25B6, 0x25B8):
            pts = [(cxs + q * 0.9, cys), (cxs - q * 0.8, cys - q), (cxs - q * 0.8, cys + q)]
        else:
            pts = [(cxs - q * 0.9, cys), (cxs + q * 0.8, cys - q), (cxs + q * 0.8, cys + q)]
        d.polygon(pts, fill=255)
    elif cp in (0x25C6, 0x25C7):
        pts = [(cxs, cys - r), (cxs + r * 0.8, cys), (cxs, cys + r), (cxs - r * 0.8, cys)]
        if cp == 0x25C7:
            d.polygon(pts, outline=255, width=lw)
        else:
            d.polygon(pts, fill=255)
    elif cp == FALLBACK_CP:
        pts = [(cxs, cys - r * 1.15), (cxs + r * 0.95, cys), (cxs, cys + r * 1.15), (cxs - r * 0.95, cys)]
        d.polygon(pts, fill=255)
        bar = max(2, int(round(w * s * 0.09)))
        d.rectangle([cxs - bar, cys - r * 0.6, cxs + bar, cys + r * 0.15], fill=0)
        d.rectangle([cxs - bar, cys + r * 0.38, cxs + bar, cys + r * 0.38 + bar * 2], fill=0)
    elif cp in (0x2713, 0x2714):
        wd = lw if cp == 0x2713 else int(lw * 1.6)
        d.line([(cxs - r * 0.8, cys + r * 0.05), (cxs - r * 0.25, cys + r * 0.6), (cxs + r * 0.85, cys - r * 0.7)], fill=255, width=wd, joint="curve")
    elif cp in (0x2717, 0x2718):
        wd = lw if cp == 0x2717 else int(lw * 1.6)
        d.line([(cxs - r * 0.7, cys - r * 0.7), (cxs + r * 0.7, cys + r * 0.7)], fill=255, width=wd)
        d.line([(cxs + r * 0.7, cys - r * 0.7), (cxs - r * 0.7, cys + r * 0.7)], fill=255, width=wd)
    elif cp in (0x2605, 0x2606):
        import math
        pts = []
        for i in range(10):
            ang = -math.pi / 2 + i * math.pi / 5
            rad = r * (1.0 if i % 2 == 0 else 0.42)
            pts.append((cxs + rad * math.cos(ang), cys + rad * math.sin(ang)))
        if cp == 0x2606:
            d.polygon(pts, outline=255, width=lw)
        else:
            d.polygon(pts, fill=255)
    elif cp in (0x2190, 0x2192, 0x2194, 0x2191, 0x2193, 0x2195):
        horizontal = cp in (0x2190, 0x2192, 0x2194)
        a = r * 1.05
        head = r * 0.5
        if horizontal:
            d.line([(cxs - a, cys), (cxs + a, cys)], fill=255, width=lw)
            if cp in (0x2192, 0x2194):
                d.line([(cxs + a, cys), (cxs + a - head, cys - head)], fill=255, width=lw)
                d.line([(cxs + a, cys), (cxs + a - head, cys + head)], fill=255, width=lw)
            if cp in (0x2190, 0x2194):
                d.line([(cxs - a, cys), (cxs - a + head, cys - head)], fill=255, width=lw)
                d.line([(cxs - a, cys), (cxs - a + head, cys + head)], fill=255, width=lw)
        else:
            a = r * 1.35
            d.line([(cxs, cys - a), (cxs, cys + a)], fill=255, width=lw)
            if cp in (0x2191, 0x2195):
                d.line([(cxs, cys - a), (cxs - head, cys - a + head)], fill=255, width=lw)
                d.line([(cxs, cys - a), (cxs + head, cys - a + head)], fill=255, width=lw)
            if cp in (0x2193, 0x2195):
                d.line([(cxs, cys + a), (cxs - head, cys + a - head)], fill=255, width=lw)
                d.line([(cxs, cys + a), (cxs + head, cys + a - head)], fill=255, width=lw)
    elif cp in (0x21B5, 0x23CE):
        a = r * 0.95
        head = r * 0.45
        d.line([(cxs + a, cys - r * 0.8), (cxs + a, cys + r * 0.15), (cxs - a, cys + r * 0.15)], fill=255, width=lw)
        d.line([(cxs - a, cys + r * 0.15), (cxs - a + head, cys + r * 0.15 - head)], fill=255, width=lw)
        d.line([(cxs - a, cys + r * 0.15), (cxs - a + head, cys + r * 0.15 + head)], fill=255, width=lw)
    elif cp == 0x20B9:
        em = w / ADVANCE_EM
        x0, x1 = w * 0.22 * s, w * 0.80 * s
        ytop = (baseline - 0.73 * em) * s
        y2 = ytop + 0.16 * em * s
        rr = 0.19 * em * s
        d.line([(x0, ytop), (x1, ytop)], fill=255, width=lw)
        d.line([(x0, y2), (x1, y2)], fill=255, width=lw)
        d.arc([x1 - 2 * rr, ytop, x1, ytop + 2 * rr], 270, 450, fill=255, width=lw)
        d.line([(x1 - rr, ytop + 2 * rr), (x0, ytop + 2 * rr)], fill=255, width=lw)
        d.line([(x0 + 0.04 * w * s, ytop + 2 * rr), (x1 - 0.02 * w * s, (baseline) * s)], fill=255, width=lw)
    elif cp in (0x2264, 0x2265):
        a = r * 0.8
        sign = -1 if cp == 0x2264 else 1
        d.line([(cxs - sign * a, cys - r * 0.45), (cxs + sign * a, cys), (cxs - sign * a, cys + r * 0.45)], fill=255, width=lw)
        d.line([(cxs - a, cys + r * 0.95), (cxs + a, cys + r * 0.95)], fill=255, width=lw)
    elif cp == 0x2260:
        a = r * 0.85
        d.line([(cxs - a, cys - r * 0.28), (cxs + a, cys - r * 0.28)], fill=255, width=lw)
        d.line([(cxs - a, cys + r * 0.28), (cxs + a, cys + r * 0.28)], fill=255, width=lw)
        d.line([(cxs - r * 0.35, cys + r * 0.8), (cxs + r * 0.35, cys - r * 0.8)], fill=255, width=lw)
    elif cp == 0x2248:
        a = r * 0.85
        for dy in (-r * 0.3, r * 0.3):
            pts = []
            for i in range(0, 25):
                t = i / 24.0
                import math
                pts.append((cxs - a + 2 * a * t, cys + dy - math.sin(t * 2 * math.pi) * r * 0.2))
            d.line(pts, fill=255, width=lw, joint="curve")
    return big.resize((w, h), Image.BOX)


SHAPES = [0x25CF, 0x25CB, 0x25E6, 0x25A0, 0x25A1, 0x25AA, 0x25AB, 0x25B2, 0x25BC, 0x25B6, 0x25C0, 0x25B8, 0x25BE, 0x25C2,
          0x25B4, 0x25C6, 0x25C7, 0x2713, 0x2714, 0x2717, 0x2718, 0x2605, 0x2606, 0x2190, 0x2192, 0x2194, 0x2195,
          0x21B5, 0x23CE, 0x20B9, 0x2264, 0x2265, 0x2260, 0x2248, FALLBACK_CP]


def synthesized_cps():
    cps = set(BOX.keys()) | set(DASHED.keys()) | set(ARCS.keys()) | {0x2571, 0x2572, 0x2573}
    cps |= set(range(0x2580, 0x25A0))
    cps |= set(range(0x2800, 0x2900))
    cps |= set(SHAPES)
    return cps


def synth_glyph(cp, w, h, baseline):
    if cp in BOX or cp in DASHED or cp in ARCS or cp in (0x2571, 0x2572, 0x2573):
        return draw_box(cp, w, h)
    if 0x2580 <= cp < 0x25A0:
        return draw_block(cp, w, h)
    if 0x2800 <= cp < 0x2900:
        return draw_braille(cp, w, h)
    return draw_shape(cp, w, h, baseline)


def build_face(fonts_reg, fonts_bold, w, h):
    em, baseline = metrics(w, h)
    loaded_reg = [(ImageFont.truetype(p, em), c) for p, c in fonts_reg]
    loaded_bold = [(ImageFont.truetype(p, em), c) for p, c in fonts_bold]
    synth = synthesized_cps()

    all_cps = set()
    for _, cmap in fonts_reg:
        all_cps |= cmap
    all_cps = {c for c in all_cps if c >= 0x20 and c != 0x7F and not (0x80 <= c < 0xA0) and c not in synth}
    all_cps |= synth
    all_cps.add(FALLBACK_CP)
    cps = sorted(all_cps)

    def find(loaded, cp):
        for f, cmap in loaded:
            if cp in cmap:
                return f
        return None

    glyphs_reg = {}
    glyphs_bold = {}
    for cp in cps:
        if cp in synth:
            img = synth_glyph(cp, w, h, baseline)
            glyphs_reg[cp] = quantize(img)
            glyphs_bold[cp] = None
            continue
        fr = find(loaded_reg, cp)
        if fr is None:
            continue
        glyphs_reg[cp] = quantize(render_font_glyph(cp, fr, w, h, baseline))
        fb = find(loaded_bold, cp)
        glyphs_bold[cp] = quantize(render_font_glyph(cp, fb, w, h, baseline)) if fb else None
    cps = [c for c in cps if c in glyphs_reg]
    return cps, glyphs_reg, glyphs_bold, baseline


def pack_glyph(px):
    out = bytearray()
    for i in range(0, len(px), 2):
        out.append(px[i] | (px[i + 1] << 4))
    return bytes(out)


def emit(path, faces):
    import struct
    header_size = 12 + 32 * len(faces)
    body = bytearray()
    descs = []

    def align4():
        while (header_size + len(body)) % 4:
            body.append(0)

    def put(data):
        align4()
        off = header_size + len(body)
        body.extend(data)
        return off

    for (w, h, baseline, cps, reg_off, bold_off, blob) in faces:
        off_cps = put(struct.pack('<%dI' % len(cps), *cps))
        off_reg = put(struct.pack('<%dI' % len(cps), *reg_off))
        off_bold = put(struct.pack('<%dI' % len(cps), *[0xFFFFFFFF if o is None else o for o in bold_off]))
        off_bits = put(blob)
        descs.append(struct.pack('<8I', w, h, baseline, len(cps), off_cps, off_reg, off_bold, off_bits))
    align4()
    out = struct.pack('<4sII', b'CRNA', 1, len(faces)) + b''.join(descs) + bytes(body)
    with open(path, 'wb') as f:
        f.write(out)


def contact_sheet(path, w, h, cps, glyphs, cols=48):
    rows = (len(cps) + cols - 1) // cols
    sheet = Image.new("L", (cols * (w + 1), rows * (h + 1)), 0)
    for i, cp in enumerate(cps):
        px = glyphs[cp]
        img = Image.new("L", (w, h))
        img.putdata([int(v * 255 / 15) for v in px])
        sheet.paste(img, ((i % cols) * (w + 1), (i // cols) * (h + 1)))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font_dir")
    ap.add_argument("out_cpp")
    ap.add_argument("--preview")
    args = ap.parse_args()

    reg = load_fonts(args.font_dir, 400)
    bold = load_fonts(args.font_dir, 700)
    faces = []
    total = 0
    for w, h in FACES:
        cps, gr, gb, baseline = build_face(reg, bold, w, h)
        blob = bytearray()
        seen = {}

        def add(px):
            data = pack_glyph(px)
            if data in seen:
                return seen[data]
            off = len(blob)
            blob.extend(data)
            seen[data] = off
            return off

        reg_off = [add(gr[c]) for c in cps]
        bold_off = [None if gb[c] is None else add(gb[c]) for c in cps]
        faces.append((w, h, baseline, cps, reg_off, bold_off, bytes(blob)))
        total += len(blob)
        print("face %dx%d baseline %d: %d glyphs, %d bytes of bitmap" % (w, h, baseline, len(cps), len(blob)))
        if args.preview:
            contact_sheet(args.preview + ".%dx%d.png" % (w, h), w, h, cps, gr)
    emit(args.out_cpp, faces)
    print("total bitmap bytes:", total)


if __name__ == "__main__":
    main()
