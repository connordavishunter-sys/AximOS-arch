"""Draws the icons for 2x, name@2x.png beside each 16x16 one in share/icons, in
the same shapes and colours, from shapes drawn at 8 times the size and scaled
down, so their edges are smooth. Run from the top of the tree: python3 tools/icons.py"""
import math, os
from PIL import Image, ImageDraw

SIZE, OVER = 32, 8  # the icon, and how much bigger it's drawn first
ICONS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'share', 'icons')

GREY, GREY_LIGHT, DIM = (150, 160, 180), (163, 174, 196), (90, 98, 116)
PAPER, PAPER_EDGE, LINE = (244, 245, 249), (156, 162, 175), (178, 183, 195)
YELLOW, YELLOW_DARK = (255, 204, 65), (222, 170, 48)
BLUE, BLUE_LIGHT, SKY, MOUNTAIN = (64, 102, 225), (90, 150, 240), (75, 140, 237), (66, 167, 120)
RED, GREEN, ORANGE, WHITE = (229, 79, 79), (78, 212, 123), (240, 120, 50), (255, 255, 255)
DARK, DARK_EDGE = (22, 26, 34), (85, 94, 113)


class Icon:
    """Drawing in the icon's own 32 units, each OVER pixels of the big one."""

    def __init__(self):
        self.image = Image.new('RGBA', (SIZE * OVER, SIZE * OVER), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.image)

    def _p(self, points):
        return [(x * OVER, y * OVER) for x, y in points]

    def rect(self, x0, y0, x1, y1, fill=None, radius=0, outline=None, width=0):
        self.d.rounded_rectangle([x0 * OVER, y0 * OVER, x1 * OVER - 1, y1 * OVER - 1], radius * OVER, fill=fill,
                                 outline=outline, width=int(width * OVER))

    def ellipse(self, x0, y0, x1, y1, fill=None, outline=None, width=0):
        self.d.ellipse([x0 * OVER, y0 * OVER, x1 * OVER - 1, y1 * OVER - 1], fill=fill, outline=outline,
                       width=int(width * OVER))

    def circle(self, cx, cy, r, **kw):
        self.ellipse(cx - r, cy - r, cx + r, cy + r, **kw)

    def poly(self, points, fill):
        self.d.polygon(self._p(points), fill=fill)

    def line(self, points, fill, width):
        self.d.line(self._p(points), fill=fill, width=int(width * OVER), joint='curve')
        for x, y in points:  # round ends
            self.circle(x, y, width / 2, fill=fill)

    def arc(self, cx, cy, r, start, end, fill, width):
        self.d.arc([(cx - r) * OVER, (cy - r) * OVER, (cx + r) * OVER, (cy + r) * OVER], start, end, fill=fill,
                   width=int(width * OVER))

    def clear(self, draw):
        """Cuts what draw draws on a mask out of what's there."""
        mask = Image.new('L', self.image.size, 0)
        was, self.d = self.d, ImageDraw.Draw(mask)
        draw(self)
        self.d = was
        alpha = self.image.getchannel('A')
        alpha.paste(0, mask=mask)
        self.image.putalpha(alpha)

    def save(self, path):
        self.image.resize((SIZE, SIZE), Image.LANCZOS).save(path)


def page(i, x0=6, x1=26, fold=7):
    i.poly([(x0, 2), (x1 - fold, 2), (x1, 2 + fold), (x1, 30), (x0, 30)], PAPER_EDGE)
    i.poly([(x0 + 1.5, 3.5), (x1 - fold - 0.5, 3.5), (x1 - 1.5, 2 + fold + 0.5), (x1 - 1.5, 28.5), (x0 + 1.5, 28.5)], PAPER)
    i.poly([(x1 - fold, 2), (x1 - fold, 2 + fold), (x1, 2 + fold)], LINE)


def lines(i, x0, x1, ys, colour=LINE, width=2):
    for y in ys:
        i.rect(x0, y, x1, y + width, colour, 0.5)


def speaker(i, colour=GREY):
    i.poly([(3, 12), (9, 12), (17, 5), (17, 27), (9, 20), (3, 20)], colour)


def cross(i, cx, cy, r, colour=RED, width=3):
    i.line([(cx - r, cy - r), (cx + r, cy + r)], colour, width)
    i.line([(cx - r, cy + r), (cx + r, cy - r)], colour, width)


def bars(i, colour, high=4):
    for n in range(4):
        h = 5 + n * 6.5
        i.rect(4 + n * 6.5, 28 - h, 8.5 + n * 6.5, 28, colour if n < high else DIM, 1)


def crescent(i, colour, shift=(7, -5)):
    i.circle(16, 16, 12, fill=colour)
    i.clear(lambda m: m.circle(16 + shift[0], 16 + shift[1], 10.5, fill=255))


def bell(i, colour=GREY):
    i.poly([(16, 3), (22, 6), (24, 12), (24, 20), (28, 25), (4, 25), (8, 20), (8, 12), (10, 6)], colour)
    i.circle(16, 4, 2.5, fill=colour)
    i.circle(16, 27.5, 3, fill=colour)


def window(i, title):
    i.rect(2, 4, 30, 28, PAPER_EDGE, 2)
    i.rect(3.5, 5.5, 28.5, 26.5, (225, 230, 240), 1)
    i.rect(2, 4, 30, 10, title, 2)
    i.rect(2, 8, 30, 10, title)


def picture(i):
    i.rect(1, 4, 31, 28, SKY, 2.5)
    i.poly([(3, 26), (13, 13), (19, 20), (22, 17), (29, 26)], MOUNTAIN)
    i.circle(23, 11, 3.5, fill=YELLOW)


def folder(i, colour, light):
    i.rect(2, 5, 14, 11, colour, 2)
    i.poly([(12, 5), (14, 5), (17, 8), (12, 8)], colour)
    i.rect(2, 8, 30, 27, colour, 2.5)
    i.rect(2, 11, 30, 27, light, 2.5)


def gear(i, colour):
    for n in range(8):
        a = n * math.pi / 4
        c, s = math.cos(a), math.sin(a)
        i.poly([(16 + c * 9 - s * 3, 16 + s * 9 + c * 3), (16 + c * 14 - s * 2.5, 16 + s * 14 + c * 2.5),
                (16 + c * 14 + s * 2.5, 16 + s * 14 - c * 2.5), (16 + c * 9 + s * 3, 16 + s * 9 - c * 3)], colour)
    i.circle(16, 16, 10.5, fill=colour)
    i.clear(lambda m: m.circle(16, 16, 4.5, fill=255))


def note(i, colour, x=15, y=8):
    i.rect(x, y, x + 2.5, y + 14, colour)
    i.poly([(x, y), (x + 8, y + 3), (x + 8, y + 7), (x, y + 4)], colour)
    i.ellipse(x - 6, y + 11, x + 2.5, y + 17.5, fill=colour)


def draw(name):
    i = Icon()
    if name == 'airplane':
        i.line([(16, 4), (16, 26)], GREY, 4)
        i.poly([(16, 11), (30, 18), (30, 21), (16, 17), (2, 21), (2, 18)], GREY)
        i.poly([(16, 23), (22, 27), (22, 29), (16, 27.5), (10, 29), (10, 27)], GREY)
    elif name == 'audio':
        page(i)
        note(i, RED, 14, 10)
    elif name == 'bell':
        bell(i)
    elif name == 'bluetooth':
        i.line([(10, 10), (22, 21), (16, 27), (16, 5), (22, 11), (10, 22)], BLUE_LIGHT, 2.5)
    elif name == 'bluetooth-off':
        i.line([(10, 10), (22, 21), (16, 27), (16, 5), (22, 11), (10, 22)], DIM, 2.5)
        cross(i, 25, 8, 4)
    elif name == 'brightness':
        i.circle(16, 16, 6.5, fill=YELLOW)
        for n in range(8):
            a = n * math.pi / 4
            i.line([(16 + math.cos(a) * 10, 16 + math.sin(a) * 10), (16 + math.cos(a) * 13, 16 + math.sin(a) * 13)],
                   YELLOW_DARK, 2.5)
    elif name == 'clipboard':
        i.rect(5, 4, 27, 30, GREY, 3)
        i.rect(7.5, 7, 24.5, 27.5, (60, 66, 80), 1.5)
        i.rect(11, 2, 21, 8, GREY_LIGHT, 2)
        lines(i, 10, 22, [12, 17, 22], GREY)
    elif name == 'drive':  # a USB stick
        i.rect(11, 2, 21, 10, GREY_LIGHT, 1.5)
        i.rect(13, 4, 15, 6.5, DIM)
        i.rect(17, 4, 19, 6.5, DIM)
        i.rect(8, 9, 24, 30, BLUE, 3)
        i.rect(12, 14, 20, 16, BLUE_LIGHT, 1)
    elif name == 'file':
        page(i)
    elif name == 'folder':
        folder(i, YELLOW_DARK, YELLOW)
    elif name == 'image' or name == 'viewer':
        picture(i)
    elif name == 'lock':
        i.arc(16, 13, 7, 180, 360, GREY_LIGHT, 3)
        i.line([(9, 13), (9, 16)], GREY_LIGHT, 3)
        i.line([(23, 13), (23, 16)], GREY_LIGHT, 3)
        i.rect(5, 15, 27, 30, YELLOW, 3)
        i.circle(16, 21, 2.5, fill=(150, 110, 30))
        i.rect(15, 21, 17, 26, (150, 110, 30), 1)
    elif name == 'logout':
        i.line([(17, 5), (6, 5), (6, 27), (17, 27)], GREY, 3)
        i.line([(12, 16), (24, 16)], BLUE_LIGHT, 3.5)
        i.poly([(21, 9), (30, 16), (21, 23)], BLUE_LIGHT)
    elif name == 'muted':
        speaker(i)
        cross(i, 25, 16, 4)
    elif name == 'network':
        bars(i, GREY_LIGHT)
    elif name == 'night':
        crescent(i, GREY_LIGHT)
    elif name == 'offline':
        bars(i, DIM, 0)
        cross(i, 24, 8, 5)
    elif name == 'program':
        window(i, BLUE)
    elif name == 'quiet':
        bell(i)
        i.clear(lambda m: m.line([(5, 3), (29, 29)], 255, 6))
        i.line([(5, 3), (28, 28)], GREY, 2.5)
    elif name == 'reboot':
        i.arc(16, 17, 11, -60, 250, GREEN, 3.5)
        i.poly([(16, 1), (24, 6), (16, 11)], GREEN)
    elif name == 'shutdown':
        i.arc(16, 17, 11, -50, 230, RED, 3.5)
        i.line([(16, 3), (16, 15)], RED, 3.5)
    elif name == 'speaker':
        speaker(i)
        i.arc(17, 16, 5, -50, 50, GREY, 2.5)
        i.arc(17, 16, 10, -50, 50, GREY, 2.5)
    elif name == 'suspend':
        crescent(i, GREY_LIGHT, (8, -3))
    elif name == 'text':
        page(i)
        lines(i, 10, 22, [11, 15, 19, 23], (150, 156, 170))
    elif name == 'theme':
        i.circle(16, 16, 12, outline=GREY, width=2.5)
        i.d.pieslice([4 * OVER, 4 * OVER, 28 * OVER, 28 * OVER], 90, 270, fill=GREY)
    elif name == 'calculator':
        i.rect(5, 2, 27, 30, (59, 66, 86), 3)
        i.rect(8, 5, 24, 11, (197, 229, 196), 1)
        for row in range(3):
            for col in range(3):
                i.rect(8 + col * 5.5, 14 + row * 5, 11.5 + col * 5.5, 17 + row * 5, PAPER if col < 2 else ORANGE, 0.8)
    elif name == 'editor':
        page(i, 4, 24)
        lines(i, 8, 19, [10, 14, 18], (150, 156, 170))
        i.line([(29, 12), (17, 26)], ORANGE, 4)
        i.poly([(15, 25), (18.5, 28), (14, 30)], (90, 70, 50))
    elif name == 'fbdemo':
        for n, colour in enumerate([(248, 76, 86), (75, 203, 118), (75, 120, 230), YELLOW]):
            i.rect(2 + n % 2 * 14.5, 2 + n // 2 * 14.5, 15.5 + n % 2 * 14.5, 15.5 + n // 2 * 14.5, colour, 2)
    elif name == 'files':
        folder(i, (60, 120, 220), BLUE_LIGHT)
        i.rect(8, 16, 24, 19, (200, 220, 255), 1)
    elif name == 'guidemo':
        i.circle(16, 16, 14, fill=(255, 214, 64))
        i.circle(11, 12, 2, fill=(70, 50, 20))
        i.circle(21, 12, 2, fill=(70, 50, 20))
        i.arc(16, 15, 8, 25, 155, (70, 50, 20), 2.5)
    elif name == 'netsurf':
        i.circle(16, 16, 14, fill=(52, 120, 220))
        i.circle(16, 16, 11, fill=(205, 228, 255))
        for x in (11, 21):
            i.line([(x, 6), (x, 26)], (52, 120, 220), 2.5)
        for y in (11, 21):
            i.line([(6, y), (26, y)], (52, 120, 220), 2.5)
    elif name == 'paint':
        i.ellipse(2, 4, 30, 28, fill=(239, 220, 189))
        i.clear(lambda m: m.circle(21, 20, 3, fill=255))
        for x, y, colour in [(9, 11, (229, 79, 79)), (16, 8.5, YELLOW), (23, 11, BLUE), (8, 19, MOUNTAIN)]:
            i.circle(x, y, 2.6, fill=colour)
    elif name == 'player':
        i.circle(16, 16, 14, fill=ORANGE)
        note(i, WHITE, 16, 8)
    elif name == 'run':
        window(i, BLUE)
        i.poly([(12, 13), (21, 18), (12, 23)], MOUNTAIN)
    elif name == 'settings':
        gear(i, GREY)
    elif name == 'snake':
        i.rect(1, 1, 31, 31, DARK, 3)
        i.line([(6, 24), (11, 24), (11, 15), (18, 15), (18, 8), (25, 8)], (60, 190, 100), 3.5)
        i.rect(22, 21, 27, 26, RED, 1)
    elif name == 'tasks':
        i.rect(1, 4, 31, 28, DARK_EDGE, 2.5)
        i.rect(3, 6, 29, 26, DARK, 1.5)
        i.line([(6, 21), (11, 15), (15, 18), (21, 10), (26, 14)], (90, 210, 110), 2)
    elif name == 'term':
        i.rect(1, 4, 31, 28, DARK_EDGE, 2.5)
        i.rect(3, 6, 29, 26, DARK, 1.5)
        i.line([(7, 11), (12, 15), (7, 19)], (90, 210, 110), 2)
        i.rect(15, 19, 24, 21.5, (200, 205, 215), 0.5)
    elif name == 'widgets':
        i.rect(1, 3, 31, 29, (44, 50, 62), 3)
        for n, at in enumerate((20, 11, 17)):
            y = 9 + n * 7
            i.rect(5, y - 1, 27, y + 1, (110, 118, 136), 1)
            i.circle(at, y, 3, fill=(90, 120, 220))
    else:
        raise SystemExit(f'no drawing for {name}')
    return i


for name in ('bluetooth', 'bluetooth-off', 'drive'):  # new ones, at 1x too
    if not os.path.exists(os.path.join(ICONS, name + '.png')):
        draw(name).image.resize((16, 16), Image.LANCZOS).save(os.path.join(ICONS, name + '.png'))

for folder_path in (ICONS, os.path.join(ICONS, 'apps')):
    for file in sorted(os.listdir(folder_path)):
        if file.endswith('.png') and '@2x' not in file:
            draw(file[:-4]).save(os.path.join(folder_path, file[:-4] + '@2x.png'))
