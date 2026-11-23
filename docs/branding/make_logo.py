"""Jade Cocoon 2's logo (python make_logo.py): the Bozos' clown (a face drawn from retro console shapes: cross and
triangle eyes, a round red nose, square teeth) dressed as Kahu, the game's hero (his red cap with its two curved horns,
his hair), before a glowing jade cocoon, on the PS2 BIOS blue with boot towers, in RecompWatcher's colours. The same
style as the other Bozos ports (DC_Recomp, TMB_Recomp...). Writes docs/branding/jc2_icon_1024.png, jc2_icon_256.png,
jc2_banner.png and the game window's icon runtime/app/jc2.ico (16 to 256 px). Generic shapes only, no game art."""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
FONT = r'D:\tmb_kit\downloads\exo2-1796e344\Exo2[wght].ttf'
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


def clown(canvas, cx, cy, r, fire=False, kahu=False):
    """The clown face, centred at (cx, cy), radius r (in canvas pixels). fire: flames for hair and no hat (Twisted Metal:
    Black's variant). toan: Kahu's cap, headband, hair and poncho (Dark Cloud's variant)."""
    lw = max(2, int(r * 0.075))
    if kahu:
        kahu_back(canvas, cx, cy, r)
        fire = True  # no puffs and no cone hat: Kahu's cap is drawn over the head below
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
    if kahu:
        kahu_front(canvas, cx, cy, r)


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
             kahu=False):
    W, H = w * SS, h * SS
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0)) if transparent else background(W, H).convert('RGBA')
    if not transparent:
        img.alpha_composite(towers(H * 0.55, W, H, H * 1.04, 16, seed=11, dim=0.55))
    cr = H * 0.25
    if kahu:
        cocoon(img, H * 0.62, H * 0.52, cr * 1.5)
    clown(img, H * 0.62, H * (0.62 if fire else 0.62 if kahu else 0.58), cr * (0.92 if fire else 0.84 if kahu else 1.0),
          fire=fire, kahu=kahu)
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


# ---- Jade Cocoon 2: the clown as Kahu, before a jade cocoon (generic shapes only, no game art) ----

# Kahu's colours, from his model in the game: a red cap with a darker band, two cream horns curving up and out, brown
# hair; and the jade of the cocoons.
CAP = (196, 46, 52)
CAP_LIT = (226, 92, 96)
CAP_DARK = (140, 28, 40)
HORN = (240, 230, 204)
HORN_SHADE = (190, 174, 140)
HAIR = (110, 62, 36)
JADE = (64, 214, 150)
JADE_DEEP = (14, 104, 82)
JADE_LIGHT = (186, 255, 222)


def cocoon(canvas, cx, cy, r):
    """A jade cocoon: a tall glowing oval, shaded from its rim, with silk threads wound across it."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    w, h = r * 0.8, r * 1.1
    for k in range(18):
        t = k / 17
        col = tuple(int(JADE_DEEP[j] + (JADE[j] - JADE_DEEP[j]) * t) for j in range(3))
        d.ellipse([cx - w * (1 - 0.45 * t), cy - h * (1 - 0.3 * t), cx + w * (1 - 0.45 * t), cy + h * (1 - 0.3 * t)],
                  fill=col + (235,))
    lw = max(2, int(r * 0.028))
    for k in range(-4, 5):
        y = cy + k * h * 0.2
        half = w * 0.95 * math.sqrt(max(0.0, 1 - (k * 0.2 + 0.1) ** 2))  # the oval's width at the thread's lowest point
        d.arc([cx - half, y - h * 0.15, cx + half, y + h * 0.15], 15, 165, fill=JADE_LIGHT + (140,), width=lw)
    d.ellipse([cx - w * 0.38, cy - h * 0.74, cx - w * 0.08, cy - h * 0.4], fill=(232, 255, 242, 150))
    glow = layer.filter(ImageFilter.GaussianBlur(r * 0.16))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)


def kahu_back(canvas, cx, cy, r):
    """What lies behind the head: the hair, wider than the head, and locks falling at the sides of the face."""
    d = ImageDraw.Draw(canvas)
    d.ellipse([cx - r * 1.12, cy - r * 0.95, cx + r * 1.12, cy + r * 0.62], fill=HAIR + (255,))
    for side in (-1, 1):
        x = cx + side * r * 0.98
        d.polygon([(x - r * 0.2, cy - r * 0.2), (x + r * 0.2, cy - r * 0.2), (x + side * r * 0.12, cy + r * 0.78)],
                  fill=HAIR + (255,))


def horn(d, cx, cy, r, side):
    """A cream horn from the cap's side, sweeping outwards and up to a point (side -1 left, 1 right)."""
    upper, lower = [], []
    for k in range(17):
        t = k / 16
        x = cx + side * (r * 0.72 + t * r * 0.82)
        y = cy - r * 0.66 + t * r * 0.12 - t * t * r * 1.15
        width = r * 0.22 * (1 - t) ** 0.8 + r * 0.012
        upper.append((x - side * width * 0.35, y - width))
        lower.append((x + side * width * 0.35, y + width))
    d.polygon(upper + lower[::-1], fill=HORN + (255,))
    d.line(lower, fill=HORN_SHADE + (255,), width=max(2, int(r * 0.05)))


def kahu_front(canvas, cx, cy, r):
    """What lies over the head: the bangs, the two horns, the red cap with its band and a jade stone."""
    d = ImageDraw.Draw(canvas)
    locks = 6
    for k in range(locks):
        x0 = cx - r * 0.8 + k * r * 1.6 / locks
        x1 = x0 + r * 1.6 / locks
        tip = cy - r * (0.34 if k % 2 else 0.44)
        d.polygon([(x0 - r * 0.04, cy - r * 0.58), (x1 + r * 0.04, cy - r * 0.58), ((x0 + x1) / 2 - r * 0.04, tip)],
                  fill=HAIR + (255,))
    for side in (-1, 1):
        horn(d, cx, cy, r, side)
    d.chord([cx - r * 1.0, cy - r * 1.42, cx + r * 1.0, cy - r * 0.18], 180, 360, fill=CAP + (255,))
    d.chord([cx - r * 0.72, cy - r * 1.3, cx + r * 0.18, cy - r * 0.7], 180, 360, fill=CAP_LIT + (255,))
    d.rounded_rectangle([cx - r * 1.04, cy - r * 0.86, cx + r * 1.04, cy - r * 0.6], radius=r * 0.1, fill=CAP_DARK + (255,))
    d.ellipse([cx - r * 0.13, cy - r * 0.86, cx + r * 0.13, cy - r * 0.6], fill=JADE + (255,))


def icon_jc2(px):
    """Jade Cocoon 2: the Bozos clown dressed as Kahu before a jade cocoon, on the team's PS2 blue."""
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.4, W, W, W * 1.02, 9))
    cocoon(bg, W * 0.5, W * 0.5, W * 0.4)
    clown(bg, W * 0.5, W * 0.6, W * 0.22, kahu=True)
    bg = scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


if __name__ == '__main__':
    icon_jc2(1024).save(os.path.join(OUT, 'jc2_icon_1024.png'))
    icon_jc2(256).save(os.path.join(OUT, 'jc2_icon_256.png'))
    wordmark(2000, 600, False, title='JADE COCOON 2', subtitle='NATIVE PC PORT  ·  THE BOZOS', kahu=True,
             title_size=0.31).save(os.path.join(OUT, 'jc2_banner.png'))
    icon_jc2(256).save(os.path.join(OUT, '..', '..', 'runtime', 'app', 'jc2.ico'),
                       sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('jc2 done')
