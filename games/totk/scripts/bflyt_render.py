"""Offline Nintendo FLYT picture/window compositor, using Pillow only.

The pane/resource structures follow https://nintendo-formats.com/libs/nw/bflyt.html
and LayoutExporterU's documented Window/Parts structures. No game data is embedded.
Texture decoding is injected by the caller, so the companion generator's BNTX
channel selectors (including BC4 masks) stay authoritative.

This is a static 2-D reference renderer, not Nintendo's runtime: text, capture /
blur textures, scissor panes, procedural shaders, indirect/TEV effects and game
bindings are omitted. X/Y rotations use an orthographic projection; Z rotation,
origins, parent-relative placement, inherited alpha, UV/SRT/wrap and vertex
colors are applied. TEV-less materials use lerp(black, white, texture), then
vertex color. Multiple ordinary texture maps are multiplied (approximation;
explicit TEV programs are not emulated). Normal windows use authored frame
sizes as a nine-slice; horizontal windows use complete left/right cap textures.
Parts recurse with their magnification and authored visibility; named embedded
pane replacements are honored, but extended user data is not interpreted.
"""
from dataclasses import dataclass, field, replace
import math
import struct

from PIL import Image, ImageChops

PANE_TAGS = (b'pan1', b'pic1', b'wnd1', b'txt1', b'prt1', b'bnd1', b'scr1', b'ali1', b'cpt1')
IDENTITY = (1, 0, 0, 0, 1, 0)
UV = ((0, 0), (1, 0), (0, 1), (1, 1))
WHITE = ((255, 255, 255, 255),) * 4


def string(data, at, size=None):
    end = len(data) if size is None else at + size
    return data[at:end].split(b'\0', 1)[0].decode('utf-8')


def unpack(fmt, data, at):
    return struct.unpack_from('<' + fmt, data, at)


def sections(data):
    if data[:4] not in (b'FLYT', b'FLAN') or data[4:6] != b'\xff\xfe':
        raise ValueError('Expected little-endian FLYT/FLAN')
    if unpack('I', data, 12)[0] != len(data):
        raise ValueError('Truncated FLYT')
    pos = unpack('H', data, 6)[0]
    for _ in range(unpack('H', data, 16)[0]):
        size = unpack('I', data, pos + 4)[0]
        if size < 8 or pos + size > len(data):
            raise ValueError('Invalid FLYT section extent')
        yield data[pos:pos + 4], data[pos:pos + size]
        pos += size


@dataclass
class Material:
    name: str
    flags: int
    black: tuple
    white: tuple
    maps: list = field(default_factory=list)
    srts: list = field(default_factory=list)
    tev: list = field(default_factory=list)


@dataclass
class Pane:
    tag: bytes
    name: str
    flags: int
    origin: int
    alpha: int
    translate: tuple
    rotate: tuple
    scale: tuple
    size: tuple
    children: list = field(default_factory=list)
    material: int = -1
    colors: tuple = WHITE
    coords: list = field(default_factory=list)
    text_colors: tuple = ()
    inflation: tuple = (0, 0, 0, 0)
    frame_size: tuple = (0, 0, 0, 0)
    window_flags: int = 0
    frames: list = field(default_factory=list)
    part: str = ''
    magnify: tuple = (1, 1)
    overrides: dict = field(default_factory=dict)


def picture_info(pane, data, at):
    pane.colors = tuple(tuple(data[at + i * 4:at + i * 4 + 4]) for i in range(4))
    pane.material = unpack('H', data, at + 16)[0]
    pane.coords = [tuple(unpack('2f', data, at + 20 + i * 32 + j * 8) for j in range(4))
                   for i in range(data[at + 18])]


def parse_pane(data):
    pane = Pane(data[:4], string(data, 12, 24), data[8], data[9], data[10],
                unpack('3f', data, 44), unpack('3f', data, 56),
                unpack('2f', data, 68), unpack('2f', data, 76))
    if pane.tag == b'pic1':
        picture_info(pane, data, 84)
    elif pane.tag == b'txt1':
        pane.material = unpack('H', data, 88)[0]
        pane.text_colors = (tuple(data[104:108]), tuple(data[108:112]))
    elif pane.tag == b'wnd1':
        pane.inflation = unpack('4h', data, 84)
        pane.frame_size = unpack('4H', data, 92)
        pane.window_flags = data[101]
        content, table = unpack('2I', data, 104)
        picture_info(pane, data, content)
        pane.frames = [unpack('HB', data, unpack('I', data, table + i * 4)[0]) for i in range(data[100])]
    elif pane.tag == b'prt1':
        count = unpack('I', data, 84)[0]
        pane.magnify = unpack('2f', data, 88)
        pane.part = string(data, 96 + count * 40).removesuffix('.bflyt')
        for i in range(count):
            at = 96 + i * 40
            name = string(data, at, 24)
            usage, basic, prop, _, info = unpack('BB2xIII', data, at + 24)
            if name:
                replacement = parse_pane(data[prop:]) if prop else None
                pane.overrides[name] = (usage, basic, replacement, data[info:info + 52] if info else None)
    return pane


class Layout:
    def __init__(self, data):
        self.size = (1920, 1080)
        self.centered = True
        self.textures, self.materials, self.roots, self.panes = [], [], [], {}
        stack, last = [self.roots], None
        for tag, block in sections(data):
            if tag == b'lyt1':
                self.centered = bool(block[8])
                self.size = unpack('2f', block, 12)
            elif tag == b'txl1':
                self.textures = [string(block, 12 + unpack('I', block, 12 + i * 4)[0]).removesuffix('.bntx')
                                 for i in range(unpack('H', block, 8)[0])]
            elif tag == b'mat1':
                for i in range(unpack('H', block, 8)[0]):
                    at = unpack('I', block, 12 + i * 4)[0]
                    flags = unpack('I', block, at + 28)[0]
                    mat = Material(string(block, at, 28), flags, tuple(block[at + 36:at + 40]), tuple(block[at + 40:at + 44]))
                    pos = at + 44
                    for _ in range(flags & 3):
                        texture, ws, wt = unpack('HBB', block, pos)
                        mat.maps.append((self.textures[texture], ws & 3, wt & 3))
                        pos += 4
                    for _ in range((flags >> 2) & 3):
                        mat.srts.append(unpack('5f', block, pos))
                        pos += 20
                    pos += ((flags >> 4) & 3) * 16
                    mat.tev = [tuple(block[pos + j * 4:pos + j * 4 + 4]) for j in range((flags >> 6) & 7)]
                    self.materials.append(mat)
            elif tag in PANE_TAGS:
                last = parse_pane(block)
                stack[-1].append(last)
                self.panes[last.name] = last
            elif tag == b'pas1':
                if last is None:
                    raise ValueError('FLYT child list without pane')
                stack.append(last.children)
            elif tag == b'pae1':
                if len(stack) == 1:
                    raise ValueError('Unbalanced FLYT pane tree')
                stack.pop()


def animation_tracks(data):
    """Yield pane/material tracks, retaining the authored Hermite slopes."""
    for tag, block in sections(data):
        if tag != b'pai1':
            continue
        count, table = unpack('HI', block, 14)
        for i in range(count):
            at = unpack('I', block, table + i*4)[0]
            name = string(block, at, 28)
            for j in range(block[at+28]):
                group = at + unpack('I', block, at+32+j*4)[0]
                kind = block[group:group+4]
                for k in range(block[group+4]):
                    track = group + unpack('I', block, group+8+k*4)[0]
                    index, target, curve = unpack('BBB', block, track)
                    n = unpack('H', block, track+4)[0]
                    keys = track + unpack('I', block, track+8)[0]
                    values = [(*unpack('fH', block, keys+q*8), 0) if curve == 1
                              else unpack('3f', block, keys+q*12) for q in range(n)]
                    yield name, kind, index, target, curve, values


def sample_keys(keys, frame, curve):
    if frame <= keys[0][0]: return keys[0][1]
    if frame >= keys[-1][0]: return keys[-1][1]
    for a, b in zip(keys, keys[1:]):
        if a[0] <= frame < b[0]:
            if curve != 2: return a[1]
            span = b[0]-a[0]
            t = (frame-a[0])/span
            return (2*t**3-3*t*t+1)*a[1] + (t**3-2*t*t+t)*span*a[2] + (-2*t**3+3*t*t)*b[1] + (t**3-t*t)*span*b[2]


def apply_animation(layout, data, frame):
    """Apply authored pane, material, SRT, visibility and texture-pattern keys."""
    textures = []
    for tag, block in sections(data):
        if tag == b'pai1':
            textures = [string(block, 20+unpack('I',block,20+i*4)[0]).removesuffix('.bntx')
                        for i in range(unpack('H',block,12)[0])]
    for name, kind, index, target, curve, keys in animation_tracks(data):
        if not keys:
            continue
        value = sample_keys(keys, frame, curve)
        pane = layout.panes.get(name)
        if pane and kind == b'FLPA' and target < 10:
            values = [*pane.translate, *pane.rotate, *pane.scale, *pane.size]
            values[target] = value
            pane.translate, pane.rotate, pane.scale, pane.size = tuple(values[:3]), tuple(values[3:6]), tuple(values[6:8]), tuple(values[8:])
        elif pane and kind == b'FLVI':
            pane.flags = (pane.flags & ~1) | int(bool(value))
        elif pane and kind == b'FLVC':
            if target == 16:
                pane.alpha = max(0, min(255, round(value)))
            elif target < 16:
                colors = [list(c) for c in pane.colors]
                colors[target//4][target%4] = max(0, min(255, round(value)))
                pane.colors = tuple(tuple(c) for c in colors)
        elif kind in (b'FLMC', b'FLTS', b'FLTP'):
            for mat in layout.materials:
                if mat.name != name:
                    continue
                if kind == b'FLMC' and target < 8:
                    colors = [*mat.black, *mat.white]
                    colors[target] = max(0, min(255, round(value)))
                    mat.black, mat.white = tuple(colors[:4]), tuple(colors[4:])
                elif kind == b'FLTS' and index < len(mat.srts) and target < 5:
                    srt = list(mat.srts[index])
                    srt[target] = value
                    mat.srts[index] = tuple(srt)
                elif kind == b'FLTP' and index < len(mat.maps) and 0 <= round(value) < len(textures):
                    _, ws, wt = mat.maps[index]
                    mat.maps[index] = (textures[round(value)], ws, wt)


def multiply(a, b):
    return (a[0]*b[0]+a[1]*b[3], a[0]*b[1]+a[1]*b[4], a[0]*b[2]+a[1]*b[5]+a[2],
            a[3]*b[0]+a[4]*b[3], a[3]*b[1]+a[4]*b[4], a[3]*b[2]+a[4]*b[5]+a[5])


def point(m, x, y):
    return (m[0]*x + m[1]*y + m[2], m[3]*x + m[4]*y + m[5])


def rect(pane):
    w, h = pane.size
    return ((-w/2, 0, -w)[pane.origin & 3], (h/2, 0, h)[(pane.origin >> 2) & 3], w, h)


def transform(pane, parent):
    x, y, _ = pane.translate
    if parent:
        left, top, w, h = rect(parent)
        x += (0, left, left + w)[(pane.origin >> 4) & 3]
        y += (0, top, top - h)[(pane.origin >> 6) & 3]
    rx, ry, rz = (math.radians(v) for v in pane.rotate)
    sx, sy = pane.scale
    c, s = math.cos(rz), math.sin(rz)
    return (c*sx*math.cos(ry), -s*sy*math.cos(rx), x,
            s*sx*math.cos(ry), c*sy*math.cos(rx), y)


def gradient(colors, size):
    image = Image.new('RGBA', (2, 2))
    image.putdata(colors)
    return image.resize(size, Image.Resampling.BILINEAR, box=(.5, .5, 1.5, 1.5))


def texture_sample(image, coords, srt, wrap, size):
    tx, ty, rotate, sx, sy = srt
    c, s = math.cos(math.radians(rotate)), math.sin(math.radians(rotate))
    coords = [((c*(u-.5)+s*(v-.5))/(sx or 1)+.5-tx,
               (-s*(u-.5)+c*(v-.5))/(sy or 1)+.5-ty) for u, v in coords]
    w, h = image.size
    lo_x = math.floor(min(u for u, v in coords))
    lo_y = math.floor(min(v for u, v in coords))
    hi_x = math.ceil(max(u for u, v in coords))
    hi_y = math.ceil(max(v for u, v in coords))
    # Build only UV cells touched by this quad. Clamp cells use edge texels;
    # repeat/mirror cells retain bilinear filtering across each tile boundary.
    hi_x, hi_y = max(hi_x, lo_x + 1), max(hi_y, lo_y + 1)
    if (hi_x-lo_x)*(hi_y-lo_y)*w*h > 32_000_000:
        factor = min(1, math.sqrt(32_000_000/((hi_x-lo_x)*(hi_y-lo_y)*w*h)))
        image = image.resize((max(1, round(w*factor)), max(1, round(h*factor))), Image.Resampling.BILINEAR)
        w, h = image.size
    sheet = Image.new('RGBA', ((hi_x-lo_x)*w, (hi_y-lo_y)*h))
    for cy in range(lo_y, hi_y):
        for cx in range(lo_x, hi_x):
            tile = image
            if wrap[0] == 0 and cx != 0:
                tile = tile.crop((0 if cx < 0 else w-1, 0, 1 if cx < 0 else w, h)).resize((w, h))
            elif wrap[0] == 2 and cx % 2:
                tile = tile.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
            if wrap[1] == 0 and cy != 0:
                tile = tile.crop((0, 0 if cy < 0 else h-1, w, 1 if cy < 0 else h)).resize((w, h))
            elif wrap[1] == 2 and cy % 2:
                tile = tile.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
            sheet.paste(tile, ((cx-lo_x)*w, (cy-lo_y)*h))
    quad = tuple(value for i in (0, 2, 3, 1) for value in ((coords[i][0]-lo_x)*w, (coords[i][1]-lo_y)*h))
    return sheet.transform(size, Image.Transform.QUAD, quad, Image.Resampling.BILINEAR)


class Renderer:
    def __init__(self, archive, texture_loader):
        self.archive, self.texture_loader = archive, texture_loader
        self.layouts, self.textures, self.surfaces = {}, {}, {}
        self.missing = set()

    def layout(self, name, animations=()):
        name = name.removesuffix('.bflyt')
        key = (name, tuple(animations))
        if key not in self.layouts:
            layout = Layout(self.archive['blyt/' + name + '.bflyt'])
            for animation, frame in animations:
                apply_animation(layout, self.archive['anim/'+animation+'.bflan'], frame)
            self.layouts[key] = layout
        return self.layouts[key]

    def texture(self, name):
        if name.startswith('FBLayout'):
            return None
        if name not in self.textures:
            self.textures[name] = self.texture_loader(name)
        return self.textures[name]

    def surface(self, layout, material, colors, coords, size, alpha):
        mat = layout.materials[material]
        key = (id(layout), material, colors, tuple(coords), size, alpha)
        if key in self.surfaces:
            return self.surfaces[key]
        image = None
        for i, (name, ws, wt) in enumerate(mat.maps):
            tex = self.texture(name)
            if tex is None:
                self.missing.add(name)
                return None  # framebuffer/dynamic capture is not ordinary artwork
            layer = texture_sample(tex, coords[min(i, len(coords)-1)] if coords else UV,
                                   mat.srts[i] if i < len(mat.srts) else (0, 0, 0, 1, 1), (ws, wt), size)
            image = layer if image is None else ImageChops.multiply(image, layer)
        if image is None:
            image = Image.new('RGBA', size, (255, 255, 255, 255))
        image = Image.merge('RGBA', [channel.point([round(b+(w-b)*v/255) for v in range(256)])
                                    for channel, b, w in zip(image.split(), mat.black, mat.white)])
        if colors != WHITE:
            image = ImageChops.multiply(image, gradient(colors, size))
        if alpha != 255:
            image.putalpha(image.getchannel('A').point([round(v*alpha/255) for v in range(256)]))
        # Small repeated parts dominate layouts; never retain full-screen buffers.
        if size[0]*size[1] <= 512*512:
            self.surfaces[key] = image
        return image

    def draw_quad(self, canvas, layout, material, colors, coords, bounds, matrix, alpha):
        if material < 0 or material >= len(layout.materials) or alpha <= 0:
            return
        left, top, width, height = bounds
        if width <= 0 or height <= 0:
            return
        # Rasterize at on-screen resolution, including pane scale, not at the
        # huge logical dimensions of zoomable map panes.
        sw = max(1, math.ceil(width*math.hypot(matrix[0], matrix[3])))
        sh = max(1, math.ceil(height*math.hypot(matrix[1], matrix[4])))
        sw, sh = min(sw, canvas.width*2), min(sh, canvas.height*2)
        corners = [point(matrix, x, y) for x, y in ((left, top), (left+width, top), (left, top-height), (left+width, top-height))]
        box = (max(0, math.floor(min(x for x, y in corners))), max(0, math.floor(min(y for x, y in corners))),
               min(canvas.width, math.ceil(max(x for x, y in corners))), min(canvas.height, math.ceil(max(y for x, y in corners))))
        if box[2] <= box[0] or box[3] <= box[1]:
            return
        image = self.surface(layout, material, colors, coords, (sw, sh), alpha)
        if image is None:
            return
        a, b, c, d, e, f = matrix
        det = a*e-b*d
        if abs(det) < 1e-9:
            return
        x, y = box[:2]
        affine = (e/det*sw/width, -b/det*sw/width, ((e*(x-c)-b*(y-f))/det-left)*sw/width,
                  d/det*sh/height, -a/det*sh/height, (top-(-d*(x-c)+a*(y-f))/det)*sh/height)
        layer = image.transform((box[2]-x, box[3]-y), Image.Transform.AFFINE, affine, Image.Resampling.BILINEAR)
        canvas.alpha_composite(layer, (x, y))

    def window(self, canvas, layout, pane, matrix, alpha):
        left, top, width, height = rect(pane)
        il, ir, it, ib = pane.inflation
        l, r, t, b = pane.frame_size
        outer = (left-il, top+it, width+il+ir, height+it+ib)
        horizontal = ((pane.window_flags >> 2) & 3) == 2
        if not pane.window_flags & 16 and not horizontal:
            self.draw_quad(canvas, layout, pane.material, pane.colors, pane.coords, (left, top, width, height), matrix, alpha)
        if not pane.frames:
            return
        # A single normal frame is an upper-left corner, mirrored to form
        # four corners; its last row/column stretches along the four edges.
        # Horizontal windows instead store a complete left cap.
        mat_index, flip = pane.frames[0]
        mat = layout.materials[mat_index]
        tex = self.texture(mat.maps[0][0]) if mat.maps else None
        if tex is None:
            return
        l, r = min(l, outer[2]/2), min(r, outer[2]/2)
        t, b = min(t, outer[3]/2), min(b, outer[3]/2)
        xs = (outer[0], outer[0]+l, outer[0]+outer[2]-r, outer[0]+outer[2])
        ys = (outer[1], outer[1]-t, outer[1]-outer[3]+b, outer[1]-outer[3])
        cells = ((0,0),(2,0),(0,2),(2,2),(1,0),(1,2),(0,1),(2,1))
        if horizontal:
            ys = (outer[1], outer[1]-outer[3])
            cells = ((0,0),(2,0),(1,0))
        for i, (cx, cy) in enumerate(cells):
            frame = min(i, len(pane.frames)-1)
            if len(pane.frames) == 4 and i >= 4:
                frame = (0, 2, 0, 1)[i-4]
            index, flip = pane.frames[frame]
            if len(pane.frames) == 1 or (len(pane.frames) == 4 and i >= 4):
                u0, u1 = ((0,1),(1,1),(1,0))[cx]
                v0, v1 = (0,1) if horizontal else ((0,1),(1,1),(1,0))[cy]
                if len(pane.frames) == 4:
                    if cx != 1: u0, u1 = 0, 1
                    if cy != 1: v0, v1 = 0, 1
                uv = ((u0,v0),(u1,v0),(u0,v1),(u1,v1))
            else:
                uv = UV
            if flip == 1: uv = tuple((1-u,v) for u,v in uv)
            elif flip == 2: uv = tuple((u,1-v) for u,v in uv)
            elif flip in (3,4,5):
                for _ in range(flip-2): uv = tuple((v,1-u) for u,v in uv)
            colors = pane.colors if pane.window_flags & 2 else WHITE
            self.draw_quad(canvas, layout, index, colors, [uv], (xs[cx],ys[cy],xs[cx+1]-xs[cx],ys[cy]-ys[cy+1]), matrix, alpha)

    def render(self, name, pane=None, size=None, view=None, force=False, hidden=(), animations=None):
        animations = animations or {}
        layout = self.layout(name, animations.get(name, ()))
        size = tuple(round(v) for v in (size or layout.size))
        canvas = Image.new('RGBA', size)
        view = view or (-layout.size[0]/2, layout.size[1]/2, *layout.size)
        x, y, w, h = view
        screen = (size[0]/w, 0, -x*size[0]/w, 0, -size[1]/h, y*size[1]/h)
        if not layout.centered and view == (-layout.size[0]/2, layout.size[1]/2, *layout.size):
            screen = (size[0]/w, 0, 0, 0, -size[1]/h, 0)
        def walk(current, node, matrix, parent, alpha, ancestry, overrides):
            if node.name in hidden:
                return
            if node.name in overrides:
                usage, basic, replacement, info = overrides[node.name]
                if replacement:
                    node = replace(replacement, children=node.children)
                elif info and basic:
                    node = replace(node, translate=unpack('3f',info,8) if basic & 1 else node.translate,
                                   rotate=unpack('3f',info,20) if basic & 2 else node.rotate,
                                   scale=unpack('2f',info,32) if basic & 4 else node.scale,
                                   size=unpack('2f',info,40) if basic & 8 else node.size,
                                   alpha=info[48] if basic & 16 else node.alpha)
            if not force and not node.flags & 1:
                return
            matrix = multiply(matrix, transform(node, parent))
            ga = round(alpha*node.alpha/255)
            if node.tag == b'pic1':
                self.draw_quad(canvas,current,node.material,node.colors,node.coords,rect(node),matrix,ga)
            elif node.tag == b'wnd1':
                self.window(canvas,current,node,matrix,ga)
            elif node.tag == b'prt1' and node.part:
                key = 'blyt/'+node.part+'.bflyt'
                if key in self.archive and node.part not in ancestry:
                    part = self.layout(node.part, animations.get(node.part, ()))
                    pm = multiply(matrix,(node.magnify[0],0,0,0,node.magnify[1],0))
                    for child in part.roots:
                        walk(part,child,pm,None,ga,ancestry+(node.part,),node.overrides)
                elif key not in self.archive:
                    self.missing.add(key)
            for child in node.children:
                walk(current,child,matrix,node,ga if node.flags & 2 else alpha,ancestry,overrides)
        for node in [layout.panes[pane]] if pane else layout.roots:
            walk(layout,node,screen,None,255,(name,),{})
        return canvas
