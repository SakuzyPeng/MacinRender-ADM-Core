"""Wall-local treatment polygons for hybrid-shoebox-v2. Coordinates are metres."""
import math

import numpy as np

UV = ((1, 2), (0, 2), (0, 1))
GRID = 1_000_000


def _cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _intersect(a, b, c, d):
    return (all(max(min(a[i], b[i]), min(c[i], d[i])) <= min(max(a[i], b[i]), max(c[i], d[i]))
                for i in range(2)) and _cross(a, b, c) * _cross(a, b, d) <= 0
            and _cross(c, d, a) * _cross(c, d, b) <= 0)


class Polygon:
    def __init__(self, vertices, limits):
        if not isinstance(vertices, list) or not 3 <= len(vertices) <= 128:
            raise ValueError("treatment polygon needs 3..128 vertices")
        points = []
        for point in vertices:
            if (not isinstance(point, list) or len(point) != 2 or
                    not all(type(x) in (int, float) and 0 <= x <= limit for x, limit in zip(point, limits))):
                raise ValueError("treatment vertex outside wall")
            points.append(tuple(math.floor(x * GRID + 0.5) for x in point))
        edges = list(zip(points, points[1:] + points[:1]))
        if len(set(points)) != len(points) or not sum(a[0] * b[1] - a[1] * b[0] for a, b in edges):
            raise ValueError("degenerate treatment polygon")
        for i, (a, b) in enumerate(edges):
            c = points[(i + 2) % len(points)]
            if _cross(a, b, c) == 0 and sum((b[k] - a[k]) * (c[k] - b[k]) for k in range(2)) < 0:
                raise ValueError("overlapping treatment edges")
            for j in range(i + 2, len(points)):
                if (i, j) != (0, len(points) - 1) and _intersect(a, b, *edges[j]):
                    raise ValueError("self-intersecting treatment polygon")
        self.edges = edges
        self.lo, self.hi = np.min(points, axis=0), np.max(points, axis=0)
        self.rect = len(points) == 4 and all(a[0] == b[0] or a[1] == b[1] for a, b in edges)

    def contains(self, x, y):
        box = (x >= self.lo[0]) & (x <= self.hi[0]) & (y >= self.lo[1]) & (y <= self.hi[1])
        if self.rect or not np.any(box):
            return box
        u, v = x[box], y[box]
        inside, boundary = np.zeros(len(u), bool), np.zeros(len(u), bool)
        for (ax, ay), (bx, by) in self.edges:
            side = (bx - ax) * (v - ay) - (by - ay) * (u - ax)
            boundary |= ((side == 0) & (u >= min(ax, bx)) & (u <= max(ax, bx)) &
                         (v >= min(ay, by)) & (v <= max(ay, by)))
            inside ^= ((ay > v) != (by > v)) & ((side > 0) == (by > ay))
        box[box] = inside | boundary
        return box


class SurfaceMap:
    def __init__(self, model, dimensions):
        if not isinstance(model, dict):
            raise ValueError("surface_model must be an object")
        materials, faces = model.get("materials"), model.get("faces")
        if (not isinstance(materials, list) or not 1 <= len(materials) <= 128
                or not isinstance(faces, list) or len(faces) != 6):
            raise ValueError("invalid material or face count")
        library = {}
        for m in materials:
            if not isinstance(m, dict):
                raise ValueError("material must be an object")
            name = m.get("id")
            if not isinstance(name, str) or not name or name in library:
                raise ValueError("invalid or duplicate material ID")
            for field in ("absorption", "scattering"):
                values = m.get(field)
                if (not isinstance(values, list) or len(values) != 8
                        or not all(type(x) in (float, int) and 0 <= x <= 1 for x in values)):
                    raise ValueError("material needs eight coefficients in [0, 1]")
            if m.get("law", "lambert") not in ("lambert", "fan"):
                raise ValueError("unknown material scattering law")
            library[name] = m
        self.regions, defaults, alpha, scatter, fans, spread, grooves = [], [], [], [], [], [], []

        def binding(item, u, v):
            if not isinstance(item, dict) or not isinstance(item.get("material"), str):
                raise ValueError("surface binding needs a material ID")
            m = library.get(item["material"])
            angle = item.get("angle_deg", 0)
            if m is None or type(angle) not in (int, float) or not -360 <= angle <= 360:
                raise ValueError("unknown material or invalid spread axis")
            a = math.radians(angle)
            axis, groove = np.zeros(3), np.zeros(3)
            axis[u], axis[v] = math.cos(a), math.sin(a)
            groove[u], groove[v] = -math.sin(a), math.cos(a)
            spread.append(axis)
            grooves.append(groove)
            alpha.append(m["absorption"])
            scatter.append(m["scattering"])
            fans.append(m.get("law") == "fan")
            return len(alpha) - 1

        for wall, face in enumerate(faces):
            u, v = UV[wall // 2]
            defaults.append(binding(face, u, v))
            patches = face.get("regions", [])
            if not isinstance(patches, list) or len(patches) > 64:
                raise ValueError("invalid treatment region count")
            regions = []
            for patch in patches:
                slot = binding(patch, u, v)
                polygon = Polygon(patch.get("polygon"), (dimensions[u], dimensions[v]))
                regions.append((polygon, slot))
            self.regions.append(regions)
        self.defaults = np.array(defaults)
        self.absorption, self.scattering = np.array(alpha), np.array(scatter)
        self.fans, self.axes, self.grooves = np.array(fans), np.array(spread), np.array(grooves)

    def bindings(self, walls, points):
        slots = self.defaults[walls].copy()
        for wall, regions in enumerate(self.regions):
            if not regions:
                continue
            indices = np.flatnonzero(walls == wall)
            if indices.size:
                u, v = UV[wall // 2]
                x = np.floor(points[indices, u] * GRID + 0.5).astype(np.int64)
                y = np.floor(points[indices, v] * GRID + 0.5).astype(np.int64)
                for polygon, slot in regions:
                    slots[indices[polygon.contains(x, y)]] = slot
        return slots

    def scatter_fan(self, incident, slots, normal_axes, upper, uniform):
        g = self.grooves[slots]
        parallel = np.clip(np.einsum("ij,ij->i", incident, g), -1., 1.)
        radius = np.sqrt(np.maximum(0., 1. - parallel**2))
        sine = 2. * uniform - 1.
        result = parallel[:, None] * g + (radius * sine)[:, None] * self.axes[slots]
        result[np.arange(len(slots)), normal_axes] = np.where(upper, -1., 1.) * radius * np.sqrt(1. - sine**2)
        return result
