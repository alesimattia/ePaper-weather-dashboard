"""
Anteprima del frame della dashboard per ogni pannello, disegnata come la
disegna il firmware: costanti di Layout_<pannello>.h e Layout_common.h, font
GFX letti dai loro header, icone di icons.h e icons_weather.h, ordine di
disegno di Weather::renderFrame() e algoritmi delle primitive di Adafruit_GFX.

Scrive, a partire dalla root del repository:
    docs/layout_<pannello>.png   il frame 1:1, incorporato dal README
    preview.svg                  il frame del 9.7", che GitHub mostra nel README
    preview_<pannello>.html      pagina locale con zoom, zone e coordinate

I dati mostrati (meteo, eventi, mail, sensore) sono di esempio. Il wallpaper
è quello di fallback ritagliato all'area cinema, cioè alla misura esatta che
il server serve al firmware. L'uscita è deterministica: rigenerare senza
modifiche al layout non cambia nessun file.

Uso:
    python tools/layout_preview.py [--fonts-dir PATH]

Dipendenza: Pillow.
    python -m pip install Pillow

@Mattia Alesi
"""
import argparse
import base64
import io
import os
import re
import sys
from datetime import datetime, timedelta
from html import escape
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DRIVERS_DIR = ROOT / "GxEPD2_SOLUM_ESL" / "src"
SVG_PANEL = "097c"

# Cartelle in cui cercare i font di Adafruit_GFX quando --fonts-dir manca: la
# variabile d'ambiente, l'installazione di arduino-cli di questa macchina e la
# cartella libraries dell'IDE Arduino.
FONTS_ENV = "ADAFRUIT_GFX_FONTS"
FONTS_DIR_CANDIDATES = (
    Path("C:/tmp/arduino/user/libraries/Adafruit_GFX_Library/Fonts"),
    Path.home() / "Documents" / "Arduino" / "libraries" / "Adafruit_GFX_Library" / "Fonts",
)

WHITE, BLACK, RED = 0, 1, 2
PALETTE = {WHITE: (255, 255, 255), BLACK: (0, 0, 0), RED: (220, 0, 0)}


def c_div(numerator, denominator):
    """Divisione intera del C, che tronca verso zero."""
    quotient = abs(numerator) // abs(denominator)
    return quotient if (numerator >= 0) == (denominator >= 0) else -quotient


def round_half_away(value):
    """lroundf(): metà arrotondata lontano da zero, come formatRoundedTemp() del firmware."""
    return int(value + 0.5) if value >= 0 else -int(-value + 0.5)


# =============================================================================
# Lettura dei sorgenti del firmware
# =============================================================================
def strip_c_comments(text):
    """Toglie i commenti C e C++ da un sorgente."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def panel_names():
    """Pannelli con un Layout_<nome>.h, nell'ordine dei nomi."""
    return sorted(path.stem[len("Layout_"):] for path in ROOT.glob("Layout_*.h") if path.stem != "Layout_common")


def driver_geometry(panel):
    """WIDTH_VISIBLE e HEIGHT dichiarati dal driver del pannello nel submodule."""
    headers = sorted(DRIVERS_DIR.glob(f"GxEPD2_SOLUM_{panel}_*.h"))
    if not headers:
        sys.exit(f"Driver del pannello {panel} non trovato in {DRIVERS_DIR}")
    text = strip_c_comments(headers[0].read_text(encoding="utf-8"))
    values = {name: value for name, value in re.findall(r"static const uint16_t (\w+) = (\w+);", text)}
    width = int(values["WIDTH"])
    visible = values.get("WIDTH_VISIBLE", "WIDTH")
    return {"Panel::WIDTH_VISIBLE": width if visible == "WIDTH" else int(visible),
            "Panel::HEIGHT": int(values["HEIGHT"])}


def read_constants(panel):
    """Valuta le costanti intere di Layout_<panel>.h e Layout_common.h nell'ordine in cui le
    vede il compilatore. Le espressioni usano solo + - * / e il ternario sulla rotazione."""
    values = driver_geometry(panel)
    text = strip_c_comments((ROOT / f"Layout_{panel}.h").read_text(encoding="utf-8")
                            + (ROOT / "Layout_common.h").read_text(encoding="utf-8"))
    for name, expression in re.findall(r"inline constexpr u?int\d+_t\s+(\w+)\s*=\s*([^;]+);", text):
        expression = expression.replace("(uint32_t)", "")
        for symbol in ("Panel::WIDTH_VISIBLE", "Panel::HEIGHT"):
            expression = expression.replace(symbol, str(values[symbol]))
        expression = re.sub(r"\(([^()]*?) % 2 == 0\) \? (\w+) : (\w+)",
                            lambda match: match.group(2) if eval(match.group(1), {}, values) % 2 == 0 else match.group(3),
                            expression)
        expression = " ".join(expression.split()).replace("/", "//")
        values[name] = eval(expression, {}, values)
    return values


def read_fonts():
    """Nome del font GFX di ogni FONT_* di Layout_common.h."""
    text = (ROOT / "Layout_common.h").read_text(encoding="utf-8")
    return dict(re.findall(r"inline constexpr const GFXfont\* FONT_(\w+)\s*=\s*&(\w+);", text))


def parse_c_bytes(text, array_name):
    """Byte di un array C cercato per nome intero, così RAINY_RED non trova WEATHER_RAINY_RED;
    il testo va passato senza commenti."""
    match = re.search(r"(?<![A-Za-z0-9_])" + re.escape(array_name), text)
    if not match:
        sys.exit(f"Array {array_name} non trovato")
    start = match.start()
    block = text[text.index("{", start) + 1:text.index("};", start)]
    return [int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]{1,2}", block)]


def read_string_array(text, array_name):
    """Stringhe di un array C di letterali, come MESI_IT e DOW_IT di Calendar.h."""
    start = text.index(array_name)
    return re.findall(r'"([^"]*)"', text[text.index("{", start):text.index("}", start)])


def read_small_icons():
    """Icone 20x20 di icons.h, per nome senza il prefisso INDOOR_ICON_."""
    text = strip_c_comments((ROOT / "icons.h").read_text(encoding="utf-8"))
    return {name: parse_c_bytes(text, f"INDOOR_ICON_{name}[")
            for name in re.findall(r"INDOOR_ICON_(\w+)\[", text)}


def read_weather_icons():
    """Lato, piani delle icone e tabella dei codici di icons_weather.h, nella forma di
    WeatherIcons::forCondition() e labelFor()."""
    text = strip_c_comments((ROOT / "icons_weather.h").read_text(encoding="utf-8"))
    side = int(re.search(r"inline constexpr int16_t SIDE = (\d+);", text).group(1))
    enum = {name: int(index) for name, index in re.findall(r"^\s+(\w+) = (\d+),", text, flags=re.M)}
    table = text[text.index("inline const Icon ICONS[]"):]
    table = table[:table.index("};")]
    icons = []
    for black, red in re.findall(r"\{ (\w+), (\w+) \}", table):
        icons.append(tuple(None if plane == "nullptr" else parse_c_bytes(text, f"{plane}[]") for plane in (black, red)))
    conditions = [(int(low), int(high), enum[day], enum[night], label) for low, high, day, night, label in
                  re.findall(r'\{ (\d+), (\d+), (\w+), (\w+), "([^"]*)" \}', text)]
    return side, icons, conditions, enum["CLOUD"]


def read_wallpaper():
    """Descrittore del wallpaper di fallback incluso dal .ino: larghezza, altezza, piano
    nero e piano rosso (None per un'immagine solo B/N)."""
    sketch = strip_c_comments((ROOT / "ePaper-weather-dashboard.ino").read_text(encoding="utf-8"))
    header = ROOT / re.search(r'#include\s+"(wallpaper/[^"]+\.h)"', sketch).group(1)
    text = strip_c_comments(header.read_text(encoding="utf-8", errors="replace"))
    descriptor = re.search(r"GxEPDImage::Descriptor \w+ = \{\s*GxEPDImage::\w+,\s*(\d+),\s*(\d+),"
                           r"\s*\(const uint8_t\*\)(\w+),\s*\(const uint8_t\*\)(\w+)", text)
    if not descriptor:
        sys.exit(f"Descrittore GxEPDImage non riconosciuto in {header.relative_to(ROOT).as_posix()}")
    width, height, black, red = descriptor.groups()
    black_plane = parse_c_bytes(text, f"{black}[] PROGMEM")
    red_plane = None if red == "nullptr" else parse_c_bytes(text, f"{red}[] PROGMEM")
    return int(width), int(height), black_plane, red_plane


# =============================================================================
# Primitive di Adafruit_GFX su un frame a tre colori
# =============================================================================
def find_fonts_dir(explicit):
    """Cartella Fonts di Adafruit_GFX: argomento, variabile d'ambiente o percorso noto."""
    candidates = [Path(explicit)] if explicit else []
    if os.environ.get(FONTS_ENV):
        candidates.append(Path(os.environ[FONTS_ENV]))
    candidates.extend(FONTS_DIR_CANDIDATES)
    for candidate in candidates:
        if (candidate / "FreeSans9pt7b.h").is_file():
            return candidate
    sys.exit("Font di Adafruit_GFX non trovati: passare --fonts-dir con la cartella Fonts della libreria "
             f"o impostare {FONTS_ENV}")


class GfxFont:
    """Font Adafruit GFX letto dal suo header: bitmap, glyph e intervallo dei caratteri."""

    def __init__(self, path):
        text = path.read_text(encoding="latin-1")
        bitmap_block = text[text.index("Bitmaps[]"):text.index("Glyphs[]")]
        bitmap_block = bitmap_block[bitmap_block.index("{") + 1:bitmap_block.index("}")]
        self.bitmap = [int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]{2}", bitmap_block)]
        glyph_block = text[text.index("Glyphs[]"):]
        glyph_block = glyph_block[glyph_block.index("{") + 1:]
        self.glyphs = [tuple(map(int, match)) for match in
                       re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", glyph_block)]
        tail = re.search(r"0x([0-9A-Fa-f]{2}),\s*0x([0-9A-Fa-f]{2}),\s*(\d+)\s*\};", text[text.rindex("GFXfont"):])
        self.first, self.last = int(tail.group(1), 16), int(tail.group(2), 16)

    def glyph(self, char):
        """Glyph del carattere, None fuori dall'intervallo del font come in Adafruit_GFX."""
        code = ord(char)
        if code < self.first or code > self.last:
            return None
        return self.glyphs[code - self.first]


class Canvas:
    """Frame a tre colori con le primitive di Adafruit_GFX che il firmware usa. Ogni pixel
    tiene l'ultimo colore scritto, come GxEPD2_3C::drawPixel()."""

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pixels = bytearray(width * height)
        self.font = None

    def pixel(self, x, y, color):
        if 0 <= x < self.width and 0 <= y < self.height:
            self.pixels[y * self.width + x] = color

    def fill_rect(self, x, y, w, h, color):
        for yy in range(max(y, 0), min(y + h, self.height)):
            row = yy * self.width
            for xx in range(max(x, 0), min(x + w, self.width)):
                self.pixels[row + xx] = color

    def hline(self, x, y, w, color):
        self.fill_rect(x, y, w, 1, color)

    def vline(self, x, y, h, color):
        self.fill_rect(x, y, 1, h, color)

    def line(self, x0, y0, x1, y1, color):
        """writeLine(): Bresenham con l'errore iniziale a metà del passo lungo."""
        steep = abs(y1 - y0) > abs(x1 - x0)
        if steep:
            x0, y0, x1, y1 = y0, x0, y1, x1
        if x0 > x1:
            x0, x1, y0, y1 = x1, x0, y1, y0
        dx, dy = x1 - x0, abs(y1 - y0)
        error, y_step, y = dx // 2, (1 if y0 < y1 else -1), y0
        for x in range(x0, x1 + 1):
            if steep:
                self.pixel(y, x, color)
            else:
                self.pixel(x, y, color)
            error -= dy
            if error < 0:
                y += y_step
                error += dx

    def draw_circle(self, x0, y0, r, color):
        f, ddf_x, ddf_y, x, y = 1 - r, 1, -2 * r, 0, r
        for px, py in ((x0, y0 + r), (x0, y0 - r), (x0 + r, y0), (x0 - r, y0)):
            self.pixel(px, py, color)
        while x < y:
            if f >= 0:
                y -= 1
                ddf_y += 2
                f += ddf_y
            x += 1
            ddf_x += 2
            f += ddf_x
            for px, py in ((x0 + x, y0 + y), (x0 - x, y0 + y), (x0 + x, y0 - y), (x0 - x, y0 - y),
                           (x0 + y, y0 + x), (x0 - y, y0 + x), (x0 + y, y0 - x), (x0 - y, y0 - x)):
                self.pixel(px, py, color)

    def _circle_helper(self, x0, y0, r, corner, color):
        f, ddf_x, ddf_y, x, y = 1 - r, 1, -2 * r, 0, r
        while x < y:
            if f >= 0:
                y -= 1
                ddf_y += 2
                f += ddf_y
            x += 1
            ddf_x += 2
            f += ddf_x
            if corner & 0x4:
                self.pixel(x0 + x, y0 + y, color)
                self.pixel(x0 + y, y0 + x, color)
            if corner & 0x2:
                self.pixel(x0 + x, y0 - y, color)
                self.pixel(x0 + y, y0 - x, color)
            if corner & 0x8:
                self.pixel(x0 - y, y0 + x, color)
                self.pixel(x0 - x, y0 + y, color)
            if corner & 0x1:
                self.pixel(x0 - y, y0 - x, color)
                self.pixel(x0 - x, y0 - y, color)

    def _fill_circle_helper(self, x0, y0, r, corners, delta, color):
        f, ddf_x, ddf_y, x, y = 1 - r, 1, -2 * r, 0, r
        px, py = x, y
        delta += 1
        while x < y:
            if f >= 0:
                y -= 1
                ddf_y += 2
                f += ddf_y
            x += 1
            ddf_x += 2
            f += ddf_x
            if x < y + 1:
                if corners & 1:
                    self.vline(x0 + x, y0 - y, 2 * y + delta, color)
                if corners & 2:
                    self.vline(x0 - x, y0 - y, 2 * y + delta, color)
            if y != py:
                if corners & 1:
                    self.vline(x0 + py, y0 - px, 2 * px + delta, color)
                if corners & 2:
                    self.vline(x0 - py, y0 - px, 2 * px + delta, color)
                py = y
            px = x

    def fill_circle(self, x0, y0, r, color):
        self.vline(x0, y0 - r, 2 * r + 1, color)
        self._fill_circle_helper(x0, y0, r, 3, 0, color)

    def draw_round_rect(self, x, y, w, h, r, color):
        r = min(r, min(w, h) // 2)
        self.hline(x + r, y, w - 2 * r, color)
        self.hline(x + r, y + h - 1, w - 2 * r, color)
        self.vline(x, y + r, h - 2 * r, color)
        self.vline(x + w - 1, y + r, h - 2 * r, color)
        self._circle_helper(x + r, y + r, r, 1, color)
        self._circle_helper(x + w - r - 1, y + r, r, 2, color)
        self._circle_helper(x + w - r - 1, y + h - r - 1, r, 4, color)
        self._circle_helper(x + r, y + h - r - 1, r, 8, color)

    def fill_round_rect(self, x, y, w, h, r, color):
        r = min(r, min(w, h) // 2)
        self.fill_rect(x + r, y, w - 2 * r, h, color)
        self._fill_circle_helper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color)
        self._fill_circle_helper(x + r, y + r, r, 2, h - 2 * r - 1, color)

    def fill_triangle(self, x0, y0, x1, y1, x2, y2, color):
        """fillTriangle(): scanline con i lati accumulati in interi e divisione del C."""
        if y0 > y1:
            x0, y0, x1, y1 = x1, y1, x0, y0
        if y1 > y2:
            x1, y1, x2, y2 = x2, y2, x1, y1
        if y0 > y1:
            x0, y0, x1, y1 = x1, y1, x0, y0
        if y0 == y2:
            a, b = min(x0, x1, x2), max(x0, x1, x2)
            self.hline(a, y0, b - a + 1, color)
            return
        dx01, dy01, dx02, dy02, dx12, dy12 = x1 - x0, y1 - y0, x2 - x0, y2 - y0, x2 - x1, y2 - y1
        sa = sb = 0
        last = y1 if y1 == y2 else y1 - 1
        y = y0
        while y <= last:
            a, b = x0 + c_div(sa, dy01), x0 + c_div(sb, dy02)
            sa += dx01
            sb += dx02
            a, b = min(a, b), max(a, b)
            self.hline(a, y, b - a + 1, color)
            y += 1
        sa, sb = dx12 * (y - y1), dx02 * (y - y0)
        while y <= y2:
            a, b = x1 + c_div(sa, dy12), x0 + c_div(sb, dy02)
            sa += dx12
            sb += dx02
            a, b = min(a, b), max(a, b)
            self.hline(a, y, b - a + 1, color)
            y += 1

    def bitmap(self, x, y, data, w, h, color):
        """drawBitmap(): bit 1 = colore, MSB a sinistra, righe paddate a byte."""
        if data is None:
            return
        stride = (w + 7) // 8
        for yy in range(h):
            for xx in range(w):
                if data[yy * stride + xx // 8] & (0x80 >> (xx & 7)):
                    self.pixel(x + xx, y + yy, color)

    def set_font(self, gfx_font):
        self.font = gfx_font

    def text_bounds(self, text):
        """getTextBounds() da (0, 0): riquadro d'inchiostro della stringa (x1, y1, w, h)."""
        min_x = min_y = 10 ** 6
        max_x = max_y = -(10 ** 6)
        cursor = 0
        for char in text:
            glyph = self.font.glyph(char)
            if glyph is None:
                continue
            _, gw, gh, advance, xo, yo = glyph
            if gw and gh:
                min_x, min_y = min(min_x, cursor + xo), min(min_y, yo)
                max_x, max_y = max(max_x, cursor + xo + gw - 1), max(max_y, yo + gh - 1)
            cursor += advance
        if max_x < min_x:
            return 0, 0, 0, 0
        return min_x, min_y, max_x - min_x + 1, max_y - min_y + 1

    def print(self, text, x, y, color):
        """setCursor(x, y) e print(text); restituisce la x del cursore dopo l'ultimo carattere."""
        cursor = x
        for char in text:
            glyph = self.font.glyph(char)
            if glyph is None:
                continue
            offset, gw, gh, advance, xo, yo = glyph
            bits = bit = 0
            for yy in range(gh):
                for xx in range(gw):
                    if not (bit & 7):
                        bits = self.font.bitmap[offset]
                        offset += 1
                    bit += 1
                    if bits & 0x80:
                        self.pixel(cursor + xo + xx, y + yo + yy, color)
                    bits = (bits << 1) & 0xFF
            cursor += advance
        return cursor

    def to_image(self):
        image = Image.new("RGB", (self.width, self.height))
        image.putdata([PALETTE[value] for value in self.pixels])
        return image


# =============================================================================
# Dati di esempio
# =============================================================================
NOW = datetime(2026, 10, 30, 14, 30)

# Slot meteo: corrente e tre previsioni a tre ore. id, notte, percepita, pop, pioggia e neve in mm/h.
SAMPLE_SLOTS = [
    {"id": 801, "night": False, "feels": 16.4, "pop": 0.0, "rain": None, "snow": None},
    {"id": 500, "night": True, "feels": 14.2, "pop": 0.62, "rain": 0.8, "snow": None},
    {"id": 211, "night": True, "feels": 12.8, "pop": 0.85, "rain": 2.4, "snow": None},
    {"id": 800, "night": True, "feels": 10.1, "pop": 0.0, "rain": None, "snow": None},
]
SAMPLE_SUN = (datetime(2026, 10, 30, 6, 55), datetime(2026, 10, 30, 17, 4))
SAMPLE_MORN_EVE = (11.0, 18.0)
# Storico della percepita a passi di mezz'ora prima dell'ora corrente.
SAMPLE_HISTORY = [(-3.0, 12.6), (-2.5, 13.1), (-2.0, 13.9), (-1.5, 14.6), (-1.0, 15.3), (-0.5, 15.9)]
SAMPLE_INDOOR = {"temp": 22.5, "hum": 45, "iaq": 74, "acc": 3, "press": 1013, "iaq_label": "Buona"}
SAMPLE_TUYA_FAILED = True
SAMPLE_EVENTS = [
    ("Riunione progetto ePaper", datetime(2026, 10, 30, 15, 0), False),
    ("Palestra", datetime(2026, 10, 30, 18, 30), False),
    ("Cena con amici", datetime(2026, 10, 31, 20, 0), False),
    ("Revisione firmware 12.2 pollici", datetime(2026, 11, 2, 9, 0), False),
    ("Ferie", datetime(2026, 11, 3), True),
    ("Dentista", datetime(2026, 11, 4, 11, 15), False),
    ("Consegna documentazione", datetime(2026, 11, 5, 17, 0), False),
]
SAMPLE_MAILS = [
    ("mario.rossi@example.com", "Riunione domani ore 10, sala B: prepara le slide", datetime(2026, 10, 30, 9, 42), True),
    ("notifiche@github.example", "[epd] Nuova issue sul layout del banner", datetime(2026, 10, 30, 8, 15), False),
    ("ordini@shop.example", "Il tuo ordine e' in consegna oggi", datetime(2026, 10, 30, 7, 30), True),
    ("newsletter@tech.example", "5 trend ESP32 da seguire", datetime(2026, 10, 29, 22, 10), False),
    ("banca@bank.example", "Estratto conto di ottobre disponibile", datetime(2026, 10, 29, 18, 2), False),
    ("luca.bianchi@example.com", "Foto del weekend", datetime(2026, 10, 29, 16, 47), False),
]


# =============================================================================
# Disegno del frame, con gli helper di Graphics.h
# =============================================================================
class Frame:
    """Un frame del firmware per un pannello: costanti, font, icone e canvas."""

    def __init__(self, panel, fonts_dir, sources):
        self.panel = panel
        self.L = read_constants(panel)
        self.fonts = {key: GfxFont(fonts_dir / f"{name}.h") for key, name in read_fonts().items()}
        self.sources = sources
        self.c = Canvas(self.L["SCREEN_W"], self.L["SCREEN_H"])

    # --- Graphics.h ----------------------------------------------------------
    def font(self, key):
        self.c.set_font(self.fonts[key])

    def text_width(self, text):
        return self.c.text_bounds(text)[2]

    def cap_height(self):
        return self.c.text_bounds("H")[3]

    def small_icon_y(self, baseline):
        return baseline - self.cap_height() // 2 - self.L["SMALL_ICON_SIZE"] // 2

    def draw_left(self, text, x, baseline, color):
        return self.c.print(text, x - self.c.text_bounds(text)[0], baseline, color)

    def draw_centered(self, text, center_x, baseline, color):
        x1, _, w, _ = self.c.text_bounds(text)
        self.c.print(text, center_x - w // 2 - x1, baseline, color)

    def draw_right(self, text, right_x, baseline, color):
        x1, _, w, _ = self.c.text_bounds(text)
        self.c.print(text, right_x - w - x1, baseline, color)

    def truncate(self, text, max_w):
        """truncateToWidth(): il prefisso parte da length-2, perde gli spazi in coda e prende "..",
        rimisurato a ogni passo."""
        if self.text_width(text) <= max_w or len(text) < 2:
            return text
        for prefix in range(len(text) - 2, -1, -1):
            candidate = text[:prefix].rstrip(" ") + ".."
            if prefix == 0 or self.text_width(candidate) <= max_w:
                return candidate
        return text

    def fieldset(self, x, y, w, h, title):
        """drawFieldsetRect(): bordo arrotondato e titolo sulla cap height, con striscia di sfondo."""
        L = self.L
        self.c.draw_round_rect(x, y, w, h, L["BOX_RADIUS"], BLACK)
        strip_x = x + L["TITLE_INSET"]
        self.c.fill_rect(strip_x - 4, y - 1, self.text_width(title) + 8, 3, WHITE)
        self.draw_left(title, strip_x, y + self.cap_height() // 2, BLACK)

    def temp_with_degree(self, number, anchor_x, baseline, color, radius, cap, centered):
        """Weather::drawTempWithDegree(): numero, cerchietto dei gradi e "C"."""
        x1, _, w_num, _ = self.c.text_bounds(number)
        x1c, _, w_c, _ = self.c.text_bounds("C")
        total = w_num + 2 + (2 * radius + 1) + 2 + w_c
        x = anchor_x - c_div(total, 2) if centered else anchor_x
        self.c.print(number, x - x1, baseline, color)
        x += w_num + 2
        self.c.draw_circle(x + radius, baseline - cap + radius, radius, color)
        x += 2 * radius + 1 + 2
        self.c.print("C", x - x1c, baseline, color)

    # --- frame -----------------------------------------------------------------
    def render(self):
        """Weather::renderFrame(): wallpaper, calendario, posta, banner su fondo bianco."""
        L = self.L
        self.c.fill_rect(0, 0, L["SCREEN_W"], L["SCREEN_H"], WHITE)
        self.draw_wallpaper()
        self.draw_calendar()
        self.draw_mail()
        self.draw_banner()
        return self.c

    def draw_wallpaper(self):
        """showImage() del fallback ritagliato all'area cinema, larga quanto la Posta: mostra
        il frame come appare con un'immagine a misura CINEMA_W x CINEMA_H, anche se l'asset
        ne sborda. Piano nero con bit 0 = nero, piano rosso con bit 0 = rosso, come lo
        compone GxEPDImage."""
        L = self.L
        width, height, black, red = self.sources["wallpaper"]
        stride = (width + 7) // 8
        for yy in range(min(height, L["CINEMA_H"])):
            for xx in range(min(width, L["CINEMA_W"])):
                index, mask = yy * stride + xx // 8, 0x80 >> (xx & 7)
                if not black[index] & mask:
                    color = BLACK
                elif red is not None and not red[index] & mask:
                    color = RED
                else:
                    color = WHITE
                self.c.pixel(L["CINEMA_X"] + xx, L["CINEMA_Y"] + yy, color)

    def draw_calendar(self):
        """Calendar::draw(): scheda col mese, griglia di sei settimane ed eventi."""
        L, c = self.L, self.c
        months, weekdays = self.sources["months"], self.sources["weekdays"]
        self.font("TITLE")
        self.fieldset(L["CAL_X"], L["CAL_Y"], L["CAL_W"], L["CAL_H"], months[NOW.month - 1])
        c.hline(L["CAL_X"] + L["BOX_PAD"], L["CAL_DIVIDER_Y"], L["CAL_W"] - 2 * L["BOX_PAD"], BLACK)

        grid_x = L["CAL_X"] + L["CAL_PAD"]
        cell_w = (L["CAL_W"] - 2 * L["CAL_PAD"]) // 7
        self.font("BODY")
        for index, weekday in enumerate(weekdays):
            self.draw_centered(weekday, grid_x + index * cell_w + cell_w // 2, L["CAL_GRID_Y"] + L["CAL_HDR_BASELINE"], BLACK)
        c.hline(L["CAL_X"] + L["BOX_PAD"], L["CAL_GRID_Y"] + L["CAL_HDR_H"], L["CAL_W"] - 2 * L["BOX_PAD"], BLACK)
        grid_top = L["CAL_GRID_Y"] + L["CAL_HDR_H"] + L["CAL_GRID_TOP_PAD"]
        first = NOW.replace(day=1)
        days = ((first + timedelta(days=32)).replace(day=1) - first).days
        for day in range(1, days + 1):
            index = first.weekday() + day - 1
            cell_x, cell_y = grid_x + (index % 7) * cell_w, grid_top + (index // 7) * L["CAL_CELL_H"]
            base = cell_y + (L["CAL_CELL_H"] + c.text_bounds(str(day))[3]) // 2
            if day == NOW.day:
                c.fill_round_rect(cell_x + 2, cell_y + 1, cell_w - 4, L["CAL_CELL_H"] - 2, L["CAL_CELL_R"], RED)
                self.draw_centered(str(day), cell_x + cell_w // 2, base, WHITE)
            else:
                self.draw_centered(str(day), cell_x + cell_w // 2, base, BLACK)

        # Calendar::MAX_EVENTS_DISPLAYED: la riga più alta da almeno 45 px fra 7, 6 e 5.
        rows = 7 if L["EVT_H"] // 7 >= 45 else 6 if L["EVT_H"] // 6 >= 45 else 5
        row_h = L["EVT_H"] // rows
        right_x = L["EVT_X"] + L["EVT_W"]
        for row in range(rows):
            top = L["EVT_Y"] + row * row_h
            title_base = top + row_h // 2 + L["EVT_TITLE_FROM_MIDDLE"]
            if row:
                c.hline(L["EVT_X"], top, L["EVT_W"], BLACK)
            self.font("BODY")
            if row >= len(SAMPLE_EVENTS):
                self.draw_left("--", L["EVT_X"], title_base, BLACK)
                continue
            title, start, all_day = SAMPLE_EVENTS[row]
            color = RED if start.date() == NOW.date() else BLACK
            date = start.strftime("%d/%m") if start.year == NOW.year else start.strftime("%d/%m/%y")
            time = "tutto gg" if all_day else start.strftime("%H:%M")
            right_w = max(self.text_width(date), self.text_width(time))
            self.draw_left(self.truncate(title, L["EVT_W"] - right_w - L["EVT_RIGHT_GAP"]), L["EVT_X"], title_base, color)
            self.draw_right(time, right_x, top + L["EVT_TIME_BASELINE"], color)
            self.draw_right(date, right_x, top + row_h - L["EVT_DATE_FROM_BOTTOM"], color)

    def draw_mail(self):
        """Mail::draw(): riquadro Posta a due colonne, busta rossa accanto alle non lette."""
        L, c = self.L, self.c
        self.font("TITLE")
        self.fieldset(L["MAIL_X"], L["MAIL_Y"], L["MAIL_W"], L["MAIL_H"], "Posta")
        grid_h = L["MAIL_ROWS_PER_COL"] * L["MAIL_ROW_H"]
        c.vline(L["MAIL_X"] + L["MAIL_COL_W"], L["MAIL_BODY_Y"] + 4, grid_h - 8, BLACK)
        for index in range(L["MAIL_MAX"]):
            col, row = index // L["MAIL_ROWS_PER_COL"], index % L["MAIL_ROWS_PER_COL"]
            cell_x = L["MAIL_X"] + col * L["MAIL_COL_W"]
            top = L["MAIL_BODY_Y"] + row * L["MAIL_ROW_H"]
            left, right = cell_x + L["BOX_PAD"], cell_x + L["MAIL_COL_W"] - L["BOX_PAD"]
            line1 = top + L["MAIL_LINE1_BASELINE"]
            line2 = line1 + L["MAIL_LINE_GAP"]
            if row:
                c.hline(left, top, right - left, BLACK)
            self.font("BODY")
            if index >= len(SAMPLE_MAILS):
                self.draw_left("--", left, line1, BLACK)
                continue
            sender, subject, received, unread = SAMPLE_MAILS[index]
            time, date = received.strftime("%H:%M"), received.strftime("%d/%m")
            right_w = max(self.text_width(time), self.text_width(date))
            self.draw_right(time, right, line1, BLACK)
            self.draw_right(date, right, line2, BLACK)
            max_w = right - left - right_w - L["MAIL_RIGHT_GAP"]
            reserve = L["SMALL_ICON_SIZE"] + L["SMALL_ICON_GAP"] if unread else 0
            sender = self.truncate(sender, max_w - reserve)
            self.draw_left(sender, left, line1, BLACK)
            if unread:
                c.bitmap(left + self.text_width(sender) + L["SMALL_ICON_GAP"], self.small_icon_y(line1),
                         self.sources["small_icons"]["MAIL"], L["SMALL_ICON_SIZE"], L["SMALL_ICON_SIZE"], RED)
            self.font("SMALL")
            self.draw_left(self.truncate(subject, max_w), left, line2, BLACK)

    def draw_banner(self):
        """Weather::drawBanner(): riquadri, badge Tuya, Interno, Meteo e Previsioni."""
        L, c = self.L, self.c
        self.font("TITLE")
        for x, w, title in ((L["INDOOR_X"], L["INDOOR_W"], "Interno"), (L["WEATHER_X"], L["WEATHER_W"], "Meteo"),
                            (L["FORECAST_X"], L["FORECAST_W"], "Previsioni")):
            self.fieldset(x, L["BANNER_Y"], w, L["BANNER_H"], title)
        if SAMPLE_TUYA_FAILED:
            self.font("SMALL")
            width = self.text_width("TUYA ERR")
            right_x = L["INDOOR_X"] + L["INDOOR_W"] - L["TITLE_INSET"]
            border_y = L["BANNER_Y"] + L["BANNER_H"] - 1
            c.fill_rect(right_x - width - 4, border_y - 1, width + 8, 3, WHITE)
            self.draw_right("TUYA ERR", right_x, border_y + self.cap_height() // 2, RED)
        self.draw_indoor()
        self.draw_block(0, L["CURRENT_BLOCK_X"], L["CURRENT_BLOCK_W"])
        self.draw_sun_column()
        for index in range(3):
            self.draw_block(1 + index, L["FORECAST_X"] + index * L["FORECAST_BLOCK_W"], L["FORECAST_BLOCK_W"])

    def draw_indoor(self):
        """Weather::renderIndoorBox(): temperatura, umidità, qualità dell'aria e pressione."""
        L, c, icons = self.L, self.c, self.sources["small_icons"]
        icon_x = L["INDOOR_ICON_X"]
        text_x = icon_x + L["SMALL_ICON_SIZE"] + L["SMALL_ICON_GAP"]
        size = L["SMALL_ICON_SIZE"]

        def row(icon, text, baseline):
            c.bitmap(icon_x, self.small_icon_y(baseline), icons[icon], size, size, BLACK)
            return self.draw_left(text, text_x, baseline, BLACK)

        self.font("LARGE")
        c.bitmap(icon_x, self.small_icon_y(L["INDOOR_ROW1_BASELINE"]), icons["TEMPERATURE"], size, size, BLACK)
        self.temp_with_degree(f"{SAMPLE_INDOOR['temp']:.1f}", text_x, L["INDOOR_ROW1_BASELINE"], RED, 2, 16, False)
        end = row("HUMIDITY", str(SAMPLE_INDOOR["hum"]), L["INDOOR_ROW2_BASELINE"])
        self.font("SMALL")
        self.draw_left("%", end + L["INDOOR_UNIT_GAP"], L["INDOOR_ROW2_BASELINE"], BLACK)
        self.font("BODY")
        end = row("AIR_QUALITY", f"{SAMPLE_INDOOR['iaq_label']} {SAMPLE_INDOOR['iaq']}", L["INDOOR_ROW3_BASELINE"])
        self.font("SMALL")
        self.draw_left(str(SAMPLE_INDOOR["acc"]), end + L["INDOOR_UNIT_GAP"],
                       L["INDOOR_ROW3_BASELINE"] + L["INDOOR_SUBSCRIPT_DROP"], BLACK)
        self.font("LARGE")
        end = row("PRESSURE", str(SAMPLE_INDOOR["press"]), L["INDOOR_ROW4_BASELINE"])
        self.font("SMALL")
        self.draw_left("mBar", end + L["INDOOR_UNIT_GAP"], L["INDOOR_ROW4_BASELINE"], BLACK)

    def draw_block(self, slot_index, block_x, block_w):
        """Weather::renderBlock(): orario, icona, percepita e riga sotto (etichetta o pioggia)."""
        L, c = self.L, self.c
        side, icons, conditions, fallback = self.sources["weather_icons"]
        slot = SAMPLE_SLOTS[slot_index]
        center_x = block_x + block_w // 2
        self.font("BODY")
        self.draw_centered((NOW + timedelta(hours=3 * slot_index)).strftime("%H:%M"), center_x, L["WX_TIME_BASELINE"], BLACK)

        row = next((cond for cond in conditions if cond[0] <= slot["id"] <= cond[1]), None)
        black, red = icons[(row[3] if slot["night"] else row[2]) if row else fallback]
        icon_x = center_x - side // 2
        icon_y = L["WX_ICON_Y"] + (L["ICON_SIZE"] - side) // 2
        c.bitmap(icon_x, icon_y, black, side, side, BLACK)
        c.bitmap(icon_x, icon_y, red, side, side, RED)

        self.font("LARGE_BOLD")
        self.temp_with_degree(str(round_half_away(slot["feels"])), center_x, L["WX_TEMP_BASELINE"], RED, 3, 18, True)

        has_rain = slot["rain"] is not None and slot["rain"] > 0
        has_snow = slot["snow"] is not None and slot["snow"] > 0
        if not has_rain and not has_snow and slot["pop"] <= 0:
            text = row[4] if row else "--"
        else:
            volume = (slot["rain"] if has_rain else 0.0) + (slot["snow"] if has_snow else 0.0)
            text = f"{round_half_away(slot['pop'] * 100)}% {volume:.1f}mm"
        max_w = block_w - 2 * L["WX_DESC_SIDE_PAD"]
        self.font("BODY")
        if self.text_width(text) > max_w:
            self.font("SMALL")
            text = self.truncate(text, max_w)
        self.draw_centered(text, center_x, L["WX_DESC_BASELINE"], BLACK)

    def draw_sun_column(self):
        """Weather::renderSunColumn(): alba e tramonto, barra mattino/sera e grafico."""
        L, c, icons = self.L, self.c, self.sources["small_icons"]
        center_x = L["SUN_COL_CENTER_X"]
        self.font("BODY")
        for icon, moment, baseline in (("SUNRISE", SAMPLE_SUN[0], L["SUN_ROW1_BASELINE"]),
                                       ("SUNSET", SAMPLE_SUN[1], L["SUN_ROW2_BASELINE"])):
            text = moment.strftime("%H:%M")
            start = center_x - c_div(L["SMALL_ICON_SIZE"] + L["SMALL_ICON_GAP"] + self.text_width(text), 2)
            c.bitmap(start, self.small_icon_y(baseline), icons[icon], L["SMALL_ICON_SIZE"], L["SMALL_ICON_SIZE"], RED)
            self.draw_left(text, start + L["SMALL_ICON_SIZE"] + L["SMALL_ICON_GAP"], baseline, BLACK)
        self.draw_temp_range_bar(center_x, L["SUN_ROW3_BASELINE"])
        self.draw_temp_chart(center_x, L["TC_TOP_Y"])

    def draw_temp_range_bar(self, center_x, baseline):
        """Weather::drawTempRangeBar(): barra, triangolo sulla percepita e cifre di mattino e sera."""
        L, c = self.L, self.c
        morn, eve = SAMPLE_MORN_EVE
        current = SAMPLE_SLOTS[0]["feels"]
        self.font("BODY")
        label_l, label_r = str(round_half_away(morn)), str(round_half_away(eve))
        x1_l, _, w_l, h_l = c.text_bounds(label_l)
        x1_r, _, w_r, h_r = c.text_bounds(label_r)
        pad, tri_half, deg_gap, deg_r = 2, 5, 2, 2
        gap = tri_half + 2
        slot_l, slot_r = w_l + deg_gap + 2 * deg_r + 1, w_r + deg_gap + 2 * deg_r + 1
        bar_left, bar_right = pad + slot_l + gap, L["TRB_W"] - pad - slot_r - gap
        bar_w = bar_right - bar_left
        cell_x = center_x - (bar_left + bar_right) // 2
        cell_y = baseline + L["TRB_CELL_Y_OFFSET"]
        c.fill_rect(cell_x + bar_left, cell_y + 7, bar_w, 4, BLACK)
        low, high = min(morn, eve), max(morn, eve)
        ratio = 0.5 if high - low < 0.5 else min(max((current - low) / (high - low), 0.0), 1.0)
        cursor = cell_x + bar_left + int(ratio * bar_w + 0.5)
        c.fill_triangle(cursor - tri_half, cell_y, cursor + tri_half, cell_y, cursor, cell_y + 7, BLACK)
        c.print(label_l, cell_x + pad - x1_l, baseline, BLACK)
        c.draw_circle(cell_x + pad + w_l + deg_gap + deg_r, baseline - h_l + deg_r + 1, deg_r, BLACK)
        right_label_x = cell_x + L["TRB_W"] - pad - slot_r
        c.print(label_r, right_label_x - x1_r, baseline, BLACK)
        c.draw_circle(right_label_x + w_r + deg_gap + deg_r, baseline - h_r + deg_r + 1, deg_r, BLACK)

    def draw_temp_chart(self, center_x, top_y):
        """Weather::drawTempChart(): Catmull-Rom sui campioni, passato nero e previsione rossa,
        tre tick con l'ora, massima e minima a sinistra, pallino sull'ora corrente."""
        L, c = self.L, self.c
        now = 0
        e0, e1 = now - 3 * 3600, now + 9 * 3600
        points = [(int(hours * 3600), value) for hours, value in SAMPLE_HISTORY if e0 <= hours * 3600 < now]
        points += [(3 * 3600 * index, slot["feels"]) for index, slot in enumerate(SAMPLE_SLOTS)]
        chart_l, chart_r = center_x - L["TC_W"] // 2 + L["TC_LABEL_W"], center_x + L["TC_W"] // 2
        axis_y = top_y + L["TC_H"] - L["TC_AXIS_PAD_BOT"]
        plot_top, plot_bottom = top_y + 6, axis_y - 1
        t_min, t_max = min(p[1] for p in points), max(p[1] for p in points)
        if t_max - t_min < 1.0:
            middle = (t_max + t_min) * 0.5
            t_min, t_max = middle - 0.5, middle + 0.5

        def map_x(epoch):
            return min(max(chart_l + c_div((chart_r - chart_l) * (epoch - e0), e1 - e0), chart_l), chart_r)

        def map_y(value):
            ratio = min(max((value - t_min) / (t_max - t_min), 0.0), 1.0)
            return min(max(plot_bottom - int(ratio * (plot_bottom - plot_top) + 0.5), plot_top), plot_bottom)

        def catmull(p0, p1, p2, p3, u):
            return 0.5 * (2 * p1 + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u
                          + (-p0 + 3 * p1 - 3 * p2 + p3) * u ** 3)

        for index in range(len(points) - 1):
            p1, p2 = points[index], points[index + 1]
            p0 = points[index - 1] if index > 0 else p1
            p3 = points[index + 2] if index + 2 < len(points) else p2
            # Come drawTempChart(): "adesso" è un vertice, quindi il colore è del segmento.
            color = BLACK if p1[0] < now else RED
            x1, x2 = map_x(p1[0]), map_x(p2[0])
            steps = max(x2 - x1, 1)
            previous = (x1, map_y(p1[1]))
            for step in range(1, steps + 1):
                u = step / steps
                current = (x1 + c_div((x2 - x1) * step, steps), map_y(catmull(p0[1], p1[1], p2[1], p3[1], u)))
                c.line(previous[0], previous[1], current[0], current[1], color)
                previous = current

        c.hline(chart_l, axis_y, chart_r - chart_l, BLACK)
        e_mid = e0 + (e1 - e0) // 2
        x_mid = map_x(e_mid)
        for x in (chart_l, x_mid, chart_r - 1):
            c.vline(x, axis_y + 1, L["TC_TICK_H"], BLACK)
        self.font("SMALL")
        label_y = axis_y + L["TC_AXIS_PAD_BOT"] - 1
        for epoch, tick_x, clamp_left, clamp_right in ((e0, chart_l, True, False), (e_mid, x_mid, False, False),
                                                       (e1, chart_r - 1, False, True)):
            text = (NOW + timedelta(seconds=epoch)).strftime("%H")
            x1, _, width, _ = c.text_bounds(text)
            text_x = tick_x - width // 2 - x1
            if clamp_left and text_x + x1 < chart_l:
                text_x = chart_l - x1
            if clamp_right and text_x + x1 + width > chart_r:
                text_x = chart_r - width - x1
            c.print(text, text_x, label_y, BLACK)
        self.draw_right(str(round_half_away(t_max)), chart_l - 3, plot_top + self.cap_height(), BLACK)
        self.draw_right(str(round_half_away(t_min)), chart_l - 3, axis_y - 1, BLACK)
        c.fill_circle(map_x(now), map_y(SAMPLE_SLOTS[0]["feels"]), L["TC_DOT_R"], BLACK)


# =============================================================================
# Uscite
# =============================================================================
def png_bytes(canvas):
    """PNG del frame, compresso e senza metadati variabili."""
    buffer = io.BytesIO()
    canvas.to_image().save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_if_changed(path, data):
    """Scrive solo se il contenuto cambia, così una rigenerazione a vuoto non tocca i file. SVG e
    HTML si confrontano a meno dei fine riga, che git converte in CRLF al checkout su Windows."""
    if path.exists():
        current = path.read_bytes()
        if path.suffix in (".svg", ".html"):
            current, data_compared = current.replace(b"\r\n", b"\n"), data.replace(b"\r\n", b"\n")
        else:
            data_compared = data
        if current == data_compared:
            return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def panel_title(panel, L):
    inches = {"097c": '9.7"', "122c": '12.2"'}.get(panel, panel)
    return f'SOLUM {inches} {L["SCREEN_W"]}w x {L["SCREEN_H"]}h'


def zones(L):
    """Zone del layout mostrate nella pagina HTML: nome, x, y, larghezza, altezza."""
    return [
        ("Wallpaper cinema", L["CINEMA_X"], L["CINEMA_Y"], L["CINEMA_W"], L["CINEMA_H"]),
        ("Posta", L["MAIL_X"], L["MAIL_Y"], L["MAIL_W"], L["MAIL_H"]),
        ("Calendario", L["CAL_X"], L["CAL_Y"], L["CAL_W"], L["CAL_H"]),
        ("Eventi", L["EVT_X"], L["EVT_Y"], L["EVT_W"], L["EVT_H"]),
        ("Interno", L["INDOOR_X"], L["BANNER_Y"], L["INDOOR_W"], L["BANNER_H"]),
        ("Meteo", L["WEATHER_X"], L["BANNER_Y"], L["WEATHER_W"], L["BANNER_H"]),
        ("Previsioni", L["FORECAST_X"], L["BANNER_Y"], L["FORECAST_W"], L["BANNER_H"]),
    ]


def svg_document(png, L, title):
    """SVG con il PNG incorporato: GitHub lo mostra nel README senza risorse esterne."""
    width, height = L["SCREEN_W"], L["SCREEN_H"]
    data = base64.b64encode(png).decode("ascii")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" '
            f'width="{width + 2}" height="{height + 2}" viewBox="0 0 {width + 2} {height + 2}">\n'
            f'  <title>{escape(title)}: anteprima generata da tools/layout_preview.py</title>\n'
            f'  <rect x="0.5" y="0.5" width="{width + 1}" height="{height + 1}" fill="#ffffff" stroke="#8a8f94"/>\n'
            f'  <image x="1" y="1" width="{width}" height="{height}" style="image-rendering:pixelated" '
            f'href="data:image/png;base64,{data}"/>\n'
            f'</svg>\n')


HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Anteprima __TITLE__</title>
<style>
  /* Pagina generata da tools/layout_preview.py: non va modificata a mano. */
  :root { --bg: #2b2d30; --fg: #d8dadc; --muted: #9a9ea3; --line: #44474b; --zone: 0, 122, 255; }
  html, body { margin: 0; background: var(--bg); color: var(--fg);
               font: 14px/1.45 system-ui, -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; }
  main { padding: 20px 16px 32px; max-width: 100%; box-sizing: border-box; }
  h1 { font-size: 16px; font-weight: 600; margin: 0 0 4px; }
  p { margin: 0 0 12px; color: var(--muted); }
  a { color: #7fb6ff; }
  .bar { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 16px; margin: 0 0 12px; }
  .bar button { background: #3a3d41; color: var(--fg); border: 1px solid var(--line); border-radius: 4px;
                padding: 3px 10px; font: inherit; cursor: pointer; }
  .bar button[aria-pressed="true"] { background: #4d6f99; border-color: #6d8fb9; }
  .bar label { cursor: pointer; }
  #cursor { font-variant-numeric: tabular-nums; color: var(--muted); min-width: 120px; }
  .viewport { overflow-x: auto; padding-bottom: 4px; }
  .screen { position: relative; width: __W__px; box-shadow: 0 0 0 1px #000, 0 10px 30px rgba(0,0,0,.45); }
  .screen img { display: block; width: 100%; height: auto; image-rendering: pixelated; }
  .zone { position: absolute; box-sizing: border-box; border: 1px dashed rgba(var(--zone), .9);
          background: rgba(var(--zone), .07); pointer-events: none; }
  .zone span { position: absolute; left: 0; top: 0; padding: 0 4px; font-size: 11px; line-height: 16px;
               background: rgba(var(--zone), .85); color: #fff; white-space: nowrap; }
  .screen.no-zones .zone { display: none; }
  table { border-collapse: collapse; margin-top: 18px; font-variant-numeric: tabular-nums; }
  th, td { text-align: left; padding: 3px 14px 3px 0; border-bottom: 1px solid var(--line); }
  th { color: var(--muted); font-weight: 500; }
  td.num { text-align: right; }
</style>
</head>
<body>
<main>
  <h1>Anteprima __TITLE__</h1>
  <p>Frame disegnato come lo disegna il firmware, dalle costanti di <code>Layout___PANEL__.h</code> e
     <code>Layout_common.h</code>, con dati di esempio e il wallpaper di fallback ritagliato all'area cinema.
     Altri pannelli: __LINKS__.</p>
  <div class="bar">
    <span>Zoom</span>
    <button type="button" data-zoom="1" aria-pressed="true">1x</button>
    <button type="button" data-zoom="1.5" aria-pressed="false">1.5x</button>
    <button type="button" data-zoom="2" aria-pressed="false">2x</button>
    <label><input type="checkbox" id="zones"> Zone del layout</label>
    <span id="cursor">x, y</span>
  </div>
  <div class="viewport">
    <div class="screen no-zones" id="screen">
      <img src="data:image/png;base64,__PNG__" width="__W__" height="__H__" alt="Frame __TITLE__">
__ZONES__
    </div>
  </div>
  <table>
    <thead><tr><th>Zona</th><th>x</th><th>y</th><th>larghezza</th><th>altezza</th></tr></thead>
    <tbody>
__ROWS__
    </tbody>
  </table>
  <p style="margin-top:14px">Immagine cinema chiesta al server: __CINEMA_W__ x __CINEMA_H__ px, due piani BWR da
     __PLANE__ byte (stride __STRIDE__), __TOTAL__ byte in tutto. Rigenerare con
     <code>python tools/layout_preview.py</code> dopo ogni modifica del layout.</p>
</main>
<script>
  const screen = document.getElementById("screen");
  const buttons = document.querySelectorAll("[data-zoom]");
  buttons.forEach(function (button) {
    button.addEventListener("click", function () {
      screen.style.width = (__W__ * Number(button.dataset.zoom)) + "px";
      buttons.forEach(function (other) { other.setAttribute("aria-pressed", String(other === button)); });
    });
  });
  document.getElementById("zones").addEventListener("change", function (event) {
    screen.classList.toggle("no-zones", !event.target.checked);
  });
  const cursor = document.getElementById("cursor");
  screen.addEventListener("mousemove", function (event) {
    const box = screen.getBoundingClientRect();
    const x = Math.floor((event.clientX - box.left) * __W__ / box.width);
    const y = Math.floor((event.clientY - box.top) * __H__ / box.height);
    cursor.textContent = "x " + x + ", y " + y;
  });
  screen.addEventListener("mouseleave", function () { cursor.textContent = "x, y"; });
</script>
</body>
</html>
"""


def html_document(panel, png, L, all_panels):
    """Pagina locale del pannello: frame 1:1 con zoom, zone del layout e coordinate."""
    width, height = L["SCREEN_W"], L["SCREEN_H"]
    title = panel_title(panel, L)
    zone_divs, rows = [], []
    for name, x, y, w, h in zones(L):
        style = (f"left:{x / width * 100:.4f}%;top:{y / height * 100:.4f}%;"
                 f"width:{w / width * 100:.4f}%;height:{h / height * 100:.4f}%")
        zone_divs.append(f'      <div class="zone" style="{style}"><span>{escape(name)}</span></div>')
        rows.append(f'      <tr><td>{escape(name)}</td><td class="num">{x}</td><td class="num">{y}</td>'
                    f'<td class="num">{w}</td><td class="num">{h}</td></tr>')
    links = ", ".join(f'<a href="preview_{other}.html">{other}</a>' for other in all_panels if other != panel) or "nessuno"
    replacements = {
        "__TITLE__": escape(title), "__PANEL__": panel, "__LINKS__": links, "__W__": str(width), "__H__": str(height),
        "__PNG__": base64.b64encode(png).decode("ascii"), "__ZONES__": "\n".join(zone_divs), "__ROWS__": "\n".join(rows),
        "__CINEMA_W__": str(L["CINEMA_W"]), "__CINEMA_H__": str(L["CINEMA_H"]), "__PLANE__": f'{L["CINEMA_PLANE_SZ"]:,}'.replace(",", "."),
        "__STRIDE__": str(L["CINEMA_STRIDE"]), "__TOTAL__": f'{L["CINEMA_TOTAL_SZ"]:,}'.replace(",", "."),
    }
    document = HTML_TEMPLATE
    for key, value in replacements.items():
        document = document.replace(key, value)
    return document


def generate(fonts_dir):
    """Rende ogni pannello e scrive PNG, SVG e pagine HTML; restituisce i file cambiati."""
    calendar = (ROOT / "Calendar.h").read_text(encoding="utf-8")
    sources = {
        "months": read_string_array(calendar, "MESI_IT["),
        "weekdays": read_string_array(calendar, "DOW_IT["),
        "small_icons": read_small_icons(),
        "weather_icons": read_weather_icons(),
        "wallpaper": read_wallpaper(),
    }
    panels = panel_names()
    changed = []
    for panel in panels:
        frame = Frame(panel, fonts_dir, sources)
        png = png_bytes(frame.render())
        outputs = {ROOT / "docs" / f"layout_{panel}.png": png,
                   ROOT / f"preview_{panel}.html": html_document(panel, png, frame.L, panels).encode("utf-8")}
        if panel == SVG_PANEL:
            outputs[ROOT / "preview.svg"] = svg_document(png, frame.L, panel_title(panel, frame.L)).encode("utf-8")
        changed += [path for path, data in outputs.items() if write_if_changed(path, data)]
    return changed


def main():
    parser = argparse.ArgumentParser(description="Anteprima del layout della dashboard per ogni pannello")
    parser.add_argument("--fonts-dir", help="cartella Fonts della libreria Adafruit_GFX")
    arguments = parser.parse_args()
    changed = generate(find_fonts_dir(arguments.fonts_dir))
    for path in changed:
        print("scritto", path.relative_to(ROOT).as_posix())
    if not changed:
        print("anteprime già aggiornate")


if __name__ == "__main__":
    main()
