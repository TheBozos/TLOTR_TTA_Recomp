"""The Bozos' logo (python make_logo.py), Twisted Metal: Black's variant (python make_logo.py tmb), Valkyrie Profile 2:
Silmeria's (python make_logo.py vp2s) and The Lord of the Rings: The Third Age's (python make_logo.py tta): a clown face
drawn from retro console shapes (cross and triangle eyes, a round red nose, square teeth), on the PS2 BIOS blue with boot
towers, in RecompWatcher's colours. Writes PNGs: the icon, the wordmark, a banner; vp2s and tta also write the game
window's icon (port/app/vp2s.ico, runtime/app/tta.ico). From TMB_Recomp's docs/branding/make_logo.py, as VP2S_Recomp has it.
The font: Exo 2 (SIL OFL 1.1), BOZOS_FONT=<path to Exo2[wght].ttf> or TMB's build kit's copy."""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
FONT = os.environ.get('BOZOS_FONT', r'D:\tmb_kit\downloads\exo2-1796e344\Exo2[wght].ttf')
SS = 4  # supersampling

INK = (232, 239, 255)
DIM = (154, 172, 207)
CROSS = (126, 166, 255)
CIRCLE = (255, 107, 122)
TRIANGLE = (79, 224, 163)
SQUARE = (255, 143, 216)
ACCENT = (183, 155, 255)


def font(size, weight):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([weight])
    except Exception:
        pass
    return f


def radial(w, h, cx, cy, r, stops):
    """A radial gradient: stops = [(t, (r,g,b)), ...], t from the centre (0) to r (1)."""
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = min(math.hypot(x - cx, y - cy) / r, 1.0)
            for i in range(len(stops) - 1):
                t0, c0 = stops[i]
                t1, c1 = stops[i + 1]
                if t <= t1:
                    k = (t - t0) / (t1 - t0) if t1 > t0 else 0
                    px[x, y] = tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3))
                    break
    return img


def background(w, h):
    small = radial(w // 8, h // 8, w // 16, h // 8 * 0.92, max(w, h) // 8 * 0.95,
                   [(0, (20, 50, 143)), (0.55, (5, 11, 38)), (1, (1, 3, 10))])
    return small.resize((w, h), Image.BICUBIC)


def towers(size, w, h, base, count, seed=7, dim=1.0):
    """The PS2 boot screen's towers: glowing translucent bars rising from the bottom."""
    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rnd = seed
    bw = w / (count * 1.6)
    for i in range(count):
        rnd = (rnd * 1103515245 + 12345) & 0x7fffffff
        frac = 0.25 + (rnd % 1000) / 1000 * 0.75
        x = (i + 0.3) * w / count
        top = base - frac * size
        alpha = int((40 + (rnd >> 10) % 60) * dim)
        d.rectangle([x, top, x + bw, h], fill=(111, 141, 255, alpha))
        d.rectangle([x, top, x + bw, top + bw * 0.25], fill=(201, 219, 255, int(alpha + 60 * dim)))
    glow = layer.filter(ImageFilter.GaussianBlur(w / 120))
    return Image.alpha_composite(glow, layer)


def glow_shape(canvas, draw_fn, blur, strength=1.0):
    """Draws a shape twice: blurred (its glow) and sharp."""
    g = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(g))
    # Blurred with premultiplied alpha, so the glow keeps the shape's colour (no dark fringe on a light background).
    pre = Image.new('RGBA', g.size)
    r_, g_, b_, a_ = g.split()
    from PIL import ImageChops
    pre = Image.merge('RGBA', [ImageChops.multiply(c, a_) for c in (r_, g_, b_)] + [a_]).filter(ImageFilter.GaussianBlur(blur))
    pr, pg, pb, pa = pre.split()
    import numpy as _np
    arr = _np.asarray(pre).astype(_np.float32)
    alpha = arr[..., 3:4]
    rgb = _np.where(alpha > 0, arr[..., :3] * 255.0 / _np.maximum(alpha, 1), 0)
    halo = Image.fromarray(_np.concatenate([_np.clip(rgb, 0, 255), alpha], axis=2).astype(_np.uint8), 'RGBA')
    if strength != 1.0:
        a = halo.getchannel('A').point(lambda v: min(255, int(v * strength)))
        halo.putalpha(a)
    canvas.alpha_composite(halo)
    canvas.alpha_composite(g)


def clown(canvas, cx, cy, r, fire=False, wings=False, ring=False):
    """The clown face, centred at (cx, cy), radius r (in canvas pixels). fire: flames for hair and no hat (Twisted Metal:
    Black's variant); wings: a winged helmet, no puffs and no hat (Valkyrie Profile 2's); ring: a dark rider's hood and a
    glowing gold ring above it, no puffs and no hat (The Third Age's)."""
    lw = max(2, int(r * 0.075))
    if fire:
        fire_hair(canvas, cx, cy, r)
    if ring:
        hood_back(canvas, cx, cy, r)
        fire = True  # no puffs, no hat (the hood's brim is drawn over the head)
    if wings:
        winged_helmet_wings(canvas, cx, cy, r)
        fire = True  # no puffs, no hat (the band is drawn after the head)
    # Hair puffs: two clouds of circles, in the cross and square colours.
    def puffs(d):
        for side, col in ((-1, CROSS), (1, SQUARE)):
            for k, (dx, dy, rr) in enumerate([(1.02, -0.28, 0.36), (1.18, 0.08, 0.34), (0.98, 0.38, 0.30), (0.78, -0.62, 0.28)]):
                x = cx + side * dx * r
                y = cy + dy * r
                d.ellipse([x - rr * r, y - rr * r, x + rr * r, y + rr * r], fill=col + (255,))
    if not fire:
        glow_shape(canvas, puffs, r * 0.12, 0.8)
    # Head.
    def head(d):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=INK + (255,))
    glow_shape(canvas, head, r * 0.08, 0.6)
    if wings:
        winged_helmet_band(canvas, cx, cy, r)
    if ring:
        hood_front(canvas, cx, cy, r)
    d = ImageDraw.Draw(canvas)
    # Small cone hat in the accent violet, a pompom on top.
    hx, hy = cx, cy - r * 0.86
    if fire:
        hx = None
    if hx is not None:
      d.polygon([(hx - r * 0.34, hy + r * 0.1), (hx + r * 0.34, hy + r * 0.1), (hx + r * 0.05, hy - r * 0.78)], fill=ACCENT + (255,))
    if hx is not None:
      d.line([(hx - r * 0.34, hy + r * 0.1), (hx + r * 0.34, hy + r * 0.1)], fill=(120, 90, 210, 255), width=lw)
    if hx is not None:
      d.ellipse([hx + r * 0.05 - r * 0.13, hy - r * 0.78 - r * 0.13, hx + r * 0.05 + r * 0.13, hy - r * 0.78 + r * 0.13], fill=TRIANGLE + (255,))
    # Eyes: a cross (left) and a triangle (right).
    ex, ey, es = r * 0.40, cy - r * 0.27, r * 0.16
    lx = cx - ex
    d.line([(lx - es, ey - es), (lx + es, ey + es)], fill=CROSS + (255,), width=int(lw * 1.5))
    d.line([(lx - es, ey + es), (lx + es, ey - es)], fill=CROSS + (255,), width=int(lw * 1.5))
    rx = cx + ex
    tri = [(rx, ey - es * 1.15), (rx + es * 1.2, ey + es * 0.9), (rx - es * 1.2, ey + es * 0.9)]
    d.polygon(tri, fill=TRIANGLE + (255,))
    k = 0.42  # the inner cut-out: a smaller triangle with the same centre
    gx, gy = rx, (tri[0][1] + tri[1][1] + tri[2][1]) / 3
    d.polygon([(gx + (x - gx) * k, gy + (y - gy) * k) for x, y in tri], fill=INK + (255,))
    # Mouth: a wide grin with square teeth.
    mw, mt, mb = r * 0.6, cy + r * 0.33, cy + r * 0.68
    d.chord([cx - mw, mt - (mb - mt), cx + mw, mb], 0, 180, fill=(40, 22, 60, 255))
    teeth = 4
    tw = mw * 2 / (teeth + 1.6)
    for i in range(teeth):
        x0 = cx - mw + tw * 0.8 + i * tw
        d.rectangle([x0 + tw * 0.08, mt + r * 0.01, x0 + tw * 0.92, mt + r * 0.13], fill=INK + (255,))
    d.arc([cx - mw, mt - (mb - mt), cx + mw, mb], 0, 180, fill=(40, 22, 60, 255), width=lw)
    # Cheeks.
    for side in (-1, 1):
        x, y, rr = cx + side * r * 0.66, cy + r * 0.24, r * 0.12
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=SQUARE + (110,))
    # The nose: the red circle, glowing.
    def nose(dd):
        nr = r * 0.19
        dd.ellipse([cx - nr, cy + r * 0.04 - nr, cx + nr, cy + r * 0.04 + nr], fill=CIRCLE + (255,))
    glow_shape(canvas, nose, r * 0.1, 1.4)
    dd = ImageDraw.Draw(canvas)
    nr = r * 0.19
    dd.ellipse([cx - nr * 0.55, cy + r * 0.04 - nr * 0.62, cx - nr * 0.15, cy + r * 0.04 - nr * 0.22], fill=(255, 220, 225, 200))


def scanlines(img, step):
    over = Image.new('RGBA', img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    for y in range(0, img.size[1], step):
        d.line([(0, y), (img.size[0], y)], fill=(0, 0, 0, 28))
    return Image.alpha_composite(img, over)


def gradient_text(size, text, fnt, c0, c1, tracking):
    """The text with a horizontal gradient and letter spacing, on a transparent layer."""
    w, h = size
    mask = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(mask)
    widths = [d.textlength(ch, font=fnt) for ch in text]
    total = sum(widths) + tracking * (len(text) - 1)
    asc, desc = fnt.getmetrics()
    x = (w - total) / 2
    y = (h - (asc + desc)) / 2
    for ch, cw in zip(text, widths):
        d.text((x, y), ch, font=fnt, fill=255)
        x += cw + tracking
    grad = Image.new('RGBA', (w, h))
    gp = ImageDraw.Draw(grad)
    for xx in range(w):
        k = xx / max(1, w - 1)
        gp.line([(xx, 0), (xx, h)], fill=tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3)) + (255,))
    grad.putalpha(mask)
    return grad, mask


def icon(px):
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.45, W, W, W * 1.02, 9))
    clown(bg, W / 2, W * 0.53, W * 0.26)
    bg = scanlines(bg, SS * 3)
    # Rounded square.
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


def wordmark(w, h, transparent, title='THE BOZOS', subtitle='PS2 RECOMPILATION  ·  BOUBOU666 & ARZAROTH', fire=False, title_size=0.33,
             wings=False, ring=False):
    W, H = w * SS, h * SS
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0)) if transparent else background(W, H).convert('RGBA')
    if not transparent:
        img.alpha_composite(towers(H * 0.55, W, H, H * 1.04, 16, seed=11, dim=0.55))
    cr = H * 0.25
    clown(img, H * 0.62, H * (0.62 if fire or wings or ring else 0.58), cr * (0.92 if fire or wings or ring else 1.0), fire=fire,
          wings=wings, ring=ring)
    ox, oy = int(H * 1.1), int(H * 0.10)
    tw, th = int(W - ox), int(H * 0.55)
    # The title as large as title_size allows, smaller when it would not fit the space right of the clown.
    size = int(H * title_size)
    while True:
        fnt = font(size, 800)
        probe = ImageDraw.Draw(Image.new('L', (1, 1)))
        tracking = size * 0.075
        width = sum(probe.textlength(ch, font=fnt) for ch in title) + tracking * (len(title) - 1)
        if width <= tw * 0.92 or size <= 8:
            break
        size = int(size * 0.95)
    text, tmask = gradient_text((tw, th), title, fnt, CROSS, ACCENT, tracking)
    # The glow from the letters' shape alone (a blurred mask in the accent colour), so no box shows around them.
    glow = Image.new('RGBA', (tw, th), ACCENT + (0,))
    glow.putalpha(tmask.filter(ImageFilter.GaussianBlur(H * 0.03)).point(lambda v: int(v * 0.7)))
    img.alpha_composite(glow, (ox, oy))
    img.alpha_composite(text, (ox, oy))
    sub = font(int(H * 0.085), 500)
    d = ImageDraw.Draw(img)
    line = subtitle
    lw = d.textlength(line, font=sub)
    bottom = tmask.getbbox()[3] if tmask.getbbox() else th
    d.text((ox + (tw - lw) / 2, oy + bottom + H * 0.07), line, font=sub, fill=DIM + (255,))
    if not transparent:
        img = scanlines(img, SS * 3)
    return img.resize((w, h), Image.LANCZOS)


# ---- per-game variants: the Bozos clown plus a landmark of the game (generic shapes only, no game art) ----

def fire_hair(canvas, cx, cy, r):
    """Flames standing up from the top half of the head, in three layers (outer red to inner yellow), glowing."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    tongues = [(-1.05, 0.55), (-0.85, 0.95), (-0.55, 1.25), (-0.2, 1.45), (0.2, 1.38), (0.55, 1.2), (0.85, 0.92), (1.05, 0.5)]
    for k, col in enumerate([(230, 60, 50), (255, 140, 60), (255, 210, 120)]):
        s = 1.0 - k * 0.24
        for i, (dx, hgt) in enumerate(tongues):
            ang = math.atan2(-1.0, dx * 0.9)
            bx = cx + dx * r * 0.92
            by = cy - math.sqrt(max(0.0, 1 - min(1.0, abs(dx)) ** 2)) * r * 0.55
            tip_x = bx + dx * r * 0.35 * s + (0.08 * r if i % 2 else -0.08 * r) * s
            tip_y = by - hgt * r * s
            w = r * 0.32 * s
            d.polygon([(bx - w, by + r * 0.25), (bx - w * 0.6, by - hgt * r * 0.45 * s), (tip_x, tip_y),
                       (bx + w * 0.6, by - hgt * r * 0.4 * s), (bx + w, by + r * 0.25)], fill=col + (255,))
    glow = layer.filter(ImageFilter.GaussianBlur(r * 0.14))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)


def icon_tmb(px):
    """Twisted Metal: Black: the Bozos clown with its head on fire (car combat), on the team's PS2 blue."""
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.4, W, W, W * 1.02, 9))
    clown(bg, W / 2, W * 0.6, W * 0.25, fire=True)
    bg = scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


FEATHER = (236, 242, 255)
SILVER = (176, 192, 226)


def winged_helmet_wings(canvas, cx, cy, r):
    """A Valkyrie's helmet wings: on each side of the head, four feathers fanning up and out, white with a silver edge,
    glowing (behind the head)."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for side in (-1, 1):
        bx, by = cx + side * r * 0.88, cy - r * 0.38  # the wing's root, at the temple
        for k, (ang, length, width) in enumerate([(62, 1.05, 0.24), (40, 1.18, 0.23), (20, 1.08, 0.21), (2, 0.86, 0.19)]):
            a = math.radians(ang)
            dx, dy = side * math.cos(a), -math.sin(a)  # up and outwards
            tip = (bx + dx * length * r, by + dy * length * r)
            nx, ny = -dy, dx  # the feather's width across its axis
            w = width * r
            mid = (bx + dx * length * r * 0.55, by + dy * length * r * 0.55)
            poly = [(bx + nx * w * 0.35, by + ny * w * 0.35), (mid[0] + nx * w, mid[1] + ny * w), tip,
                    (mid[0] - nx * w * 0.7, mid[1] - ny * w * 0.7), (bx - nx * w * 0.35, by - ny * w * 0.35)]
            d.polygon(poly, fill=SILVER + (255,))
            inner = [(mid[0] + (x - mid[0]) * 0.8, mid[1] + (y - mid[1]) * 0.8) for x, y in poly]
            d.polygon(inner, fill=FEATHER + (255,))
    glow = layer.filter(ImageFilter.GaussianBlur(r * 0.1))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)


def winged_helmet_band(canvas, cx, cy, r):
    """The helmet itself: a silver cap over the top of the head with a band across the brow and a violet gem."""
    d = ImageDraw.Draw(canvas)
    d.chord([cx - r, cy - r, cx + r, cy + r], 200, 340, fill=SILVER + (255,))
    band = r * 0.13
    d.arc([cx - r, cy - r, cx + r, cy + r], 200, 340, fill=(120, 140, 190, 255), width=int(band * 0.5))
    gy = cy - r * 0.62
    g = r * 0.13
    d.polygon([(cx, gy - g), (cx + g * 0.8, gy), (cx, gy + g), (cx - g * 0.8, gy)], fill=ACCENT + (255,))
    d.polygon([(cx, gy - g * 0.5), (cx + g * 0.35, gy), (cx, gy - g * 0.05)], fill=(232, 220, 255, 255))


def icon_vp2s(px):
    """Valkyrie Profile 2: Silmeria: the Bozos clown in a winged Valkyrie helmet, on the team's PS2 blue."""
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.4, W, W, W * 1.02, 9))
    clown(bg, W / 2, W * 0.58, W * 0.24, wings=True)
    bg = scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


# The Third Age's colours: a Black Rider's hood (near-black with a cold blue rim, the PS2 blue showing through) and the One
# Ring's gold.
HOOD = (14, 16, 26)
HOOD_RIM = (64, 82, 130)
GOLD = (255, 196, 72)
GOLD_LIT = (255, 236, 160)


def hood_back(canvas, cx, cy, r):
    """The rider's hood behind the head: a pointed cowl wider than the face, falling to the shoulders, a dim blue rim."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cowl = [(cx, cy - r * 1.75), (cx + r * 0.95, cy - r * 1.05), (cx + r * 1.38, cy - r * 0.05), (cx + r * 1.5, cy + r * 1.25),
            (cx + r * 0.9, cy + r * 1.5), (cx - r * 0.9, cy + r * 1.5), (cx - r * 1.5, cy + r * 1.25), (cx - r * 1.38, cy - r * 0.05),
            (cx - r * 0.95, cy - r * 1.05)]
    d.polygon(cowl, fill=HOOD_RIM + (255,))
    inner = [(cx + (x - cx) * 0.94, cy + (y - cy) * 0.94 + r * 0.05) for x, y in cowl]
    d.polygon(inner, fill=HOOD + (255,))
    glow = layer.filter(ImageFilter.GaussianBlur(r * 0.12))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)


def hood_front(canvas, cx, cy, r):
    """The hood's brim over the top of the face (the face shows from the brow down), then the One Ring above the point:
    a gold band seen at an angle, glowing."""
    d = ImageDraw.Draw(canvas)
    d.chord([cx - r * 1.04, cy - r * 1.04, cx + r * 1.04, cy + r * 1.04], 212, 328, fill=HOOD + (255,))
    d.arc([cx - r * 1.04, cy - r * 1.04, cx + r * 1.04, cy + r * 1.04], 212, 328, fill=HOOD_RIM + (255,), width=max(2, int(r * 0.05)))

    def ring_shape(dd):
        rx, ry, y0 = r * 0.5, r * 0.3, cy - r * 2.08
        band = r * 0.16
        dd.ellipse([cx - rx, y0 - ry, cx + rx, y0 + ry], outline=GOLD + (255,), width=int(band))
    glow_shape(canvas, ring_shape, r * 0.12, 1.3)
    d = ImageDraw.Draw(canvas)
    rx, ry, y0 = r * 0.5, r * 0.3, cy - r * 2.08
    d.arc([cx - rx, y0 - ry, cx + rx, y0 + ry], 200, 300, fill=GOLD_LIT + (255,), width=max(2, int(r * 0.035)))


def icon_tta(px):
    """The Lord of the Rings: The Third Age: the Bozos clown in a dark rider's hood under the One Ring, on the team's PS2
    blue."""
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.4, W, W, W * 1.02, 9))
    clown(bg, W / 2, W * 0.66, W * 0.2, ring=True)
    bg = scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == 'tta':
    # The Third Age's logo: the team clown in a rider's hood under the One Ring; the window's icon at the usual Windows sizes.
    big = icon_tta(1024)
    big.save(os.path.join(OUT, 'tta_icon_1024.png'))
    icon_tta(256).save(os.path.join(OUT, 'tta_icon_256.png'))
    app = os.path.join(os.path.dirname(os.path.dirname(OUT)), 'runtime', 'app')
    os.makedirs(app, exist_ok=True)
    big.save(os.path.join(app, 'tta.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    wordmark(2000, 600, False, title='THE LORD OF THE RINGS: THE THIRD AGE', subtitle='NATIVE PC PORT  ·  THE BOZOS',
             title_size=0.22, ring=True).save(os.path.join(OUT, 'tta_banner.png'))
    print('tta done')


elif __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == 'vp2s':
    # Valkyrie Profile 2's logo: the team clown in a winged helmet; the window's icon at the usual Windows sizes.
    big = icon_vp2s(1024)
    big.save(os.path.join(OUT, 'vp2s_icon_1024.png'))
    icon_vp2s(256).save(os.path.join(OUT, 'vp2s_icon_256.png'))
    app = os.path.join(os.path.dirname(os.path.dirname(OUT)), 'port', 'app')
    os.makedirs(app, exist_ok=True)
    big.save(os.path.join(app, 'vp2s.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    wordmark(2000, 600, False, title='VALKYRIE PROFILE 2: SILMERIA', subtitle='NATIVE PC PORT  ·  THE BOZOS', fire=False,
             title_size=0.24, wings=True).save(os.path.join(OUT, 'vp2s_banner.png'))
    print('vp2s done')
elif __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == 'tmb':
    # Twisted Metal: Black's launcher logo: the team clown with its head on fire.
    icon_tmb(1024).save(os.path.join(OUT, 'tmb_icon_1024.png'))
    icon_tmb(256).save(os.path.join(OUT, 'tmb_icon_256.png'))
    wordmark(2000, 600, False, title='TWISTED METAL: BLACK', subtitle='NATIVE PC PORT  ·  THE BOZOS', fire=True,
             title_size=0.3).save(os.path.join(OUT, 'tmb_banner.png'))
    print('tmb done')
elif __name__ == '__main__':
    icon(1024).save(os.path.join(OUT, 'bozos_icon_1024.png'))
    icon(256).save(os.path.join(OUT, 'bozos_icon_256.png'))
    wordmark(2000, 600, False).save(os.path.join(OUT, 'bozos_banner.png'))
    wordmark(2000, 600, True).save(os.path.join(OUT, 'bozos_wordmark_transparent.png'))
    print('done')
