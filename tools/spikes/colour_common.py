"""Shared colour maths for the view-project spikes (throwaway).

OKLab/OKLCH (Ottosson 2020), CIEDE2000 (Sharma 2005), Machado 2009 CVD
matrices at severity 1.0, applied in linear sRGB.
"""

import math

import numpy as np


def srgb_to_linear(c):
    c = np.asarray(c, dtype=float)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.asarray(c, dtype=float)
    return np.where(c <= 0.0031308, 12.92 * c,
                    1.055 * np.abs(c) ** (1 / 2.4) - 0.055)


_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])


def linear_to_oklab(rgb):
    lms = _M1 @ np.asarray(rgb, dtype=float)
    return _M2 @ np.cbrt(lms)


def oklab_to_linear(lab):
    lms_ = np.linalg.solve(_M2, np.asarray(lab, dtype=float))
    return np.linalg.solve(_M1, lms_ ** 3)


def oklch_to_lab(L, C, h_deg):
    h = math.radians(h_deg)
    return np.array([L, C * math.cos(h), C * math.sin(h)])


def oklch_to_srgb(L, C, h_deg):
    """Returns (srgb, in_gamut)."""
    lin = oklab_to_linear(oklch_to_lab(L, C, h_deg))
    ok = bool(np.all(lin >= -1e-6) and np.all(lin <= 1 + 1e-6))
    return np.clip(linear_to_srgb(np.clip(lin, 0, 1)), 0, 1), ok


def srgb_to_oklab(rgb):
    return linear_to_oklab(srgb_to_linear(rgb))


def srgb_to_oklch(rgb):
    L, a, b = srgb_to_oklab(rgb)
    return L, math.hypot(a, b), math.degrees(math.atan2(b, a)) % 360


def max_chroma(L, h_deg):
    lo, hi = 0.0, 0.5
    for _ in range(40):
        mid = (lo + hi) / 2
        if oklch_to_srgb(L, mid, h_deg)[1]:
            lo = mid
        else:
            hi = mid
    return lo


_RGB2XYZ = np.array([[0.4124564, 0.3575761, 0.1804375],
                     [0.2126729, 0.7151522, 0.0721750],
                     [0.0193339, 0.1191920, 0.9503041]])
_WHITE = np.array([0.95047, 1.0, 1.08883])


def srgb_to_cielab(rgb):
    xyz = _RGB2XYZ @ srgb_to_linear(rgb) / _WHITE
    f = np.where(xyz > (6 / 29) ** 3, np.cbrt(xyz),
                 xyz / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.array([116 * f[1] - 16, 500 * (f[0] - f[1]),
                     200 * (f[1] - f[2])])


def ciede2000(lab1, lab2):
    L1, a1, b1 = lab1
    L2, a2, b2 = lab2
    C1, C2 = math.hypot(a1, b1), math.hypot(a2, b2)
    Cb = (C1 + C2) / 2
    G = 0.5 * (1 - math.sqrt(Cb ** 7 / (Cb ** 7 + 25 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360
    h2p = math.degrees(math.atan2(b2, a2p)) % 360
    dLp = L2 - L1
    dCp = C2p - C1p
    if C1p * C2p == 0:
        dhp = 0
    elif abs(h2p - h1p) <= 180:
        dhp = h2p - h1p
    elif h2p - h1p > 180:
        dhp = h2p - h1p - 360
    else:
        dhp = h2p - h1p + 360
    dHp = 2 * math.sqrt(C1p * C2p) * math.sin(math.radians(dhp / 2))
    Lbp = (L1 + L2) / 2
    Cbp = (C1p + C2p) / 2
    if C1p * C2p == 0:
        hbp = h1p + h2p
    elif abs(h1p - h2p) <= 180:
        hbp = (h1p + h2p) / 2
    elif h1p + h2p < 360:
        hbp = (h1p + h2p + 360) / 2
    else:
        hbp = (h1p + h2p - 360) / 2
    T = (1 - 0.17 * math.cos(math.radians(hbp - 30))
         + 0.24 * math.cos(math.radians(2 * hbp))
         + 0.32 * math.cos(math.radians(3 * hbp + 6))
         - 0.20 * math.cos(math.radians(4 * hbp - 63)))
    dtheta = 30 * math.exp(-(((hbp - 275) / 25) ** 2))
    Rc = 2 * math.sqrt(Cbp ** 7 / (Cbp ** 7 + 25 ** 7))
    Sl = 1 + 0.015 * (Lbp - 50) ** 2 / math.sqrt(20 + (Lbp - 50) ** 2)
    Sc = 1 + 0.045 * Cbp
    Sh = 1 + 0.015 * Cbp * T
    Rt = -math.sin(math.radians(2 * dtheta)) * Rc
    return math.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2
                     + Rt * (dCp / Sc) * (dHp / Sh))


def de2000_rgb(c1, c2):
    return ciede2000(srgb_to_cielab(c1), srgb_to_cielab(c2))


def deok_rgb(c1, c2):
    return float(np.linalg.norm(srgb_to_oklab(c1) - srgb_to_oklab(c2)))


# Machado, Oliveira & Fernandes 2009, severity 1.0, linear RGB.
MACHADO = {
    "protanopia": np.array([[0.152286, 1.052583, -0.204868],
                            [0.114503, 0.786281, 0.099216],
                            [-0.003882, -0.048116, 1.051998]]),
    "deuteranopia": np.array([[0.367322, 0.860646, -0.227968],
                              [0.280085, 0.672501, 0.047413],
                              [-0.011820, 0.042940, 0.968881]]),
    "tritanopia": np.array([[1.255528, -0.076749, -0.178779],
                            [-0.078411, 0.930809, 0.147602],
                            [0.004733, 0.691367, 0.303900]]),
}


def simulate(rgb, kind):
    lin = MACHADO[kind] @ srgb_to_linear(rgb)
    return np.clip(linear_to_srgb(np.clip(lin, 0, 1)), 0, 1)


def hexrgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255]) / 255.0


def to8(rgb):
    return tuple(int(round(float(v) * 255)) for v in rgb)
