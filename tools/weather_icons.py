"""
Genera le icone meteo del firmware a partire da Material Symbols Rounded.

Il flusso ha due passi. "extract" rende ogni glyph elencato in
icons/weather/icons.json come icona base: un PNG 512x512 a tre colori (bianco,
nero, rosso) che si può aprire, ritoccare o sostituire con un disegno proprio.
"convert" riduce le icone base al lato richiesto e scrive icons_weather.h, con
un piano nero e un piano rosso 1bpp per icona e la tabella che associa i codici
condizione di OpenWeather alle icone.

Uso:
    python tools/weather_icons.py extract [--font PATH]
    python tools/weather_icons.py convert [--side 88]

Le funzioni sono indipendenti dalla riga di comando, così un'interfaccia
grafica può importarle e chiamarle direttamente.

Dipendenza: Pillow.
    python -m pip install Pillow
"""

import argparse
import hashlib
import json
import sys
from collections import deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Tutto è relativo alla root del progetto, la cartella superiore a tools/.
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
ICONS_DIR = PROJECT_DIR / "icons" / "weather"
SPEC_PATH = ICONS_DIR / "icons.json"
DEFAULT_FONT_PATH = ICONS_DIR / "source" / "MaterialSymbolsRounded.ttf"
DEFAULT_HEADER_PATH = PROJECT_DIR / "icons_weather.h"

BASE_SIDE = 512
DEFAULT_SIDE = 88

WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
RED = (255, 0, 0)


def load_spec(spec_path=SPEC_PATH):
    """Legge icons.json e verifica che ogni condizione nomini un'icona esistente."""
    spec = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    names = {icon["name"] for icon in spec["icons"]}
    for condition in spec["conditions"]:
        for key in ("day", "night"):
            if condition[key] not in names:
                raise ValueError(f"condizione {condition['ids']}: icona '{condition[key]}' non elencata")
    if spec["fallback"] not in names:
        raise ValueError(f"fallback '{spec['fallback']}' non elencato")
    return spec


def file_sha256(path):
    """SHA256 esadecimale di un file, per verificare che il font sia quello dichiarato."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def open_font(font_path, axes, size):
    """Apre il font variabile e fissa gli assi nell'ordine Fill, Grade, Optical Size, Weight."""
    font = ImageFont.truetype(str(font_path), size)
    font.set_variation_by_axes([axes["fill"], axes["grade"], axes["optical_size"], axes["weight"]])
    return font


def render_glyph_mask(font, codepoint, side):
    """Rende il glyph centrato sulle metriche del font, così la posizione resta quella
    disegnata da Material, e restituisce una maschera "L" con 255 dove c'è inchiostro."""
    canvas = Image.new("L", (side, side), 0)
    ImageDraw.Draw(canvas).text((side / 2, side / 2), chr(codepoint), font=font, fill=255, anchor="mm")
    return canvas.point(lambda value: 255 if value >= 128 else 0)


def _label_components(pixels, width, height, target, neighbours):
    """Etichetta le componenti connesse dei pixel uguali a target; restituisce la lista
    delle etichette (0 = fuori) e il numero di componenti."""
    labels = [0] * (width * height)
    count = 0
    for start in range(width * height):
        if pixels[start] != target or labels[start]:
            continue
        count += 1
        labels[start] = count
        queue = deque([start])
        while queue:
            index = queue.popleft()
            x, y = index % width, index // width
            for dx, dy in neighbours:
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    neighbour = ny * width + nx
                    if pixels[neighbour] == target and not labels[neighbour]:
                        labels[neighbour] = count
                        queue.append(neighbour)
    return labels, count


FOUR = ((1, 0), (-1, 0), (0, 1), (0, -1))
EIGHT = FOUR + ((1, 1), (1, -1), (-1, 1), (-1, -1))


def nearest_component_labels(filled_mask):
    """Per ogni pixel, l'etichetta della sagoma piena più vicina (distanza a scacchiera),
    calcolata con una visita in ampiezza che parte da tutte le sagome insieme."""
    width, height = filled_mask.size
    pixels = list(filled_mask.tobytes())
    labels, _ = _label_components(pixels, width, height, 255, EIGHT)
    nearest = list(labels)
    queue = deque(index for index, label in enumerate(labels) if label)
    while queue:
        index = queue.popleft()
        x, y = index % width, index // width
        for dx, dy in EIGHT:
            nx, ny = x + dx, y + dy
            if 0 <= nx < width and 0 <= ny < height:
                neighbour = ny * width + nx
                if not nearest[neighbour]:
                    nearest[neighbour] = nearest[index]
                    queue.append(neighbour)
    return nearest, labels


def split_accent(mask, rule, filled_mask=None, seed=None):
    """Divide la maschera d'inchiostro in piano nero e piano rosso secondo la regola
    dell'icona: "none" tutto nero, "all" tutto rosso, "outside_shape" nero solo la
    forma indicata dal seme (coordinate 0..1 sulla nuvola o sul monte) e rosso il resto.

    Nella variante piena dei glyph Material le forme sono staccate da un taglio bianco,
    mentre nella variante vuota il contorno del sole si salda a quello della nuvola:
    ogni pixel vuoto va quindi alla forma piena più vicina, e la saldatura si divide a
    metà strada fra le due."""
    empty = Image.new("L", mask.size, 0)
    if rule == "none":
        return mask, empty
    if rule == "all":
        return empty, mask
    if rule != "outside_shape":
        raise ValueError(f"regola accento sconosciuta: {rule}")
    width, height = mask.size
    nearest, labels = nearest_component_labels(filled_mask)
    seed_index = int(seed[1] * (height - 1)) * width + int(seed[0] * (width - 1))
    if not labels[seed_index]:
        raise ValueError(f"il seme {seed} non cade su una forma piena")
    main = labels[seed_index]
    ink = mask.tobytes()
    black, red = Image.new("L", mask.size), Image.new("L", mask.size)
    black.putdata([255 if ink[i] and nearest[i] == main else 0 for i in range(width * height)])
    red.putdata([255 if ink[i] and nearest[i] != main else 0 for i in range(width * height)])
    return black, red


def compose_base_icon(black, red):
    """Compone i due piani in un PNG a tre colori; il rosso prevale dove i piani si toccano."""
    image = Image.new("RGB", black.size, WHITE)
    image.paste(BLACK, mask=black)
    image.paste(RED, mask=red)
    return image


def extract(spec, font_path=DEFAULT_FONT_PATH, out_dir=ICONS_DIR, side=BASE_SIDE):
    """Rende le icone base di icons.json in out_dir; restituisce i percorsi scritti."""
    expected = spec["source"]["sha256"]
    actual = file_sha256(font_path)
    if actual != expected:
        raise ValueError(f"il font {font_path} ha SHA256 {actual}, icons.json dichiara {expected}")
    axes = spec["source"]["axes"]
    font = open_font(font_path, axes, side)
    filled_font = open_font(font_path, {**axes, "fill": 1}, side)
    written = []
    for icon in spec["icons"]:
        codepoint = int(icon["codepoint"], 16)
        mask = render_glyph_mask(font, codepoint, side)
        filled_mask = render_glyph_mask(filled_font, codepoint, side) if icon["accent"] == "outside_shape" else None
        black, red = split_accent(mask, icon["accent"], filled_mask, icon.get("shape_seed"))
        path = Path(out_dir) / f"{icon['name']}.png"
        compose_base_icon(black, red).save(path, optimize=True)
        written.append(path)
    return written


def load_base_icon(path):
    """Legge un'icona base e la separa nei piani nero e rosso, tollerando ritocchi fatti
    con editor che sfumano i bordi: rosso è ciò che è chiaramente rosso, nero ciò che è scuro."""
    image = Image.open(path).convert("RGB")
    raw = image.tobytes()
    black, red = [], []
    for offset in range(0, len(raw), 3):
        r, g, b = raw[offset], raw[offset + 1], raw[offset + 2]
        is_red = r >= 128 and g < 128 and b < 128
        is_black = not is_red and (r + g + b) < 384
        black.append(255 if is_black else 0)
        red.append(255 if is_red else 0)
    black_image, red_image = Image.new("L", image.size), Image.new("L", image.size)
    black_image.putdata(black)
    red_image.putdata(red)
    return black_image, red_image


def scale_plane(plane, side):
    """Riduce un piano al lato richiesto con media d'area e soglia a metà."""
    reduced = plane.resize((side, side), Image.BOX)
    return reduced.point(lambda value: 255 if value >= 128 else 0)


def pack_plane(plane):
    """Impacchetta un piano nel formato di Adafruit_GFX::drawBitmap(): bit 1 = colore,
    MSB a sinistra, ogni riga paddata a byte. Restituisce None se il piano è vuoto."""
    width, height = plane.size
    pixels = plane.load()
    data = bytearray()
    for y in range(height):
        for byte_x in range(0, width, 8):
            value = 0
            for bit in range(8):
                x = byte_x + bit
                if x < width and pixels[x, y]:
                    value |= 0x80 >> bit
            data.append(value)
    return bytes(data) if any(data) else None


def icon_planes(spec, base_dir=ICONS_DIR, side=DEFAULT_SIDE):
    """Piani ridotti di ogni icona, nell'ordine di icons.json: lista di (nome, nero, rosso)
    con i piani come immagini "L"; il nero si toglie dove c'è rosso."""
    result = []
    for icon in spec["icons"]:
        black, red = load_base_icon(Path(base_dir) / f"{icon['name']}.png")
        black, red = scale_plane(black, side), scale_plane(red, side)
        black = Image.composite(Image.new("L", black.size, 0), black, red)
        result.append((icon["name"], black, red))
    return result


def _c_array(identifier, data):
    """Formatta un array PROGMEM, 16 byte per riga."""
    lines = [f"  static const uint8_t {identifier}[] PROGMEM = {{"]
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in data[offset:offset + 16])
        lines.append(f"    {chunk},")
    lines.append("  };")
    return "\n".join(lines)


def build_header(spec, planes, side):
    """Testo completo di icons_weather.h."""
    source = spec["source"]
    names = [name for name, _, _ in planes]
    index_of = {name: index for index, name in enumerate(names)}
    arrays, entries = [], []
    for name, black, red in planes:
        packed = {"BLACK": pack_plane(black), "RED": pack_plane(red)}
        pointers = []
        for plane_name in ("BLACK", "RED"):
            data = packed[plane_name]
            if data is None:
                pointers.append("nullptr")
                continue
            identifier = f"{name.upper()}_{plane_name}"
            arrays.append(_c_array(identifier, data))
            pointers.append(identifier)
        entries.append(f"    {{ {pointers[0]}, {pointers[1]} }},   // {name}")
    enum_lines = [f"    {name.upper()} = {index}," for index, name in enumerate(names)]
    condition_lines = [
        f"    {{ {c['ids'][0]}, {c['ids'][1]}, {c['day'].upper()}, {c['night'].upper()} }},"
        for c in spec["conditions"]
    ]
    return f"""#ifndef ICONS_WEATHER_H
#define ICONS_WEATHER_H

// File generato da tools/weather_icons.py a partire dalle icone base in
// icons/weather/: non va modificato a mano, si rigenera con
// "python tools/weather_icons.py convert --side {side}".

#include <stdint.h>
#include <pgmspace.h>

/**
 * Icone meteo {source['name']} ({source['license']}) associate ai codici
 * condizione di OpenWeather. Ogni icona ha un piano nero e un piano rosso nel
 * formato di Adafruit_GFX::drawBitmap(), cioè bit 1 = colore, MSB a sinistra
 * e righe paddate a byte; un piano vuoto vale nullptr.
 */
namespace WeatherIcons
{{
  /** Lato in pixel di tutte le bitmap. */
  inline constexpr int16_t SIDE = {side};

  struct Icon
  {{
    const uint8_t* black;
    const uint8_t* red;
  }};

  /** Intervallo di codici condizione e indici delle icone di giorno e di notte. */
  struct Condition
  {{
    uint16_t idMin;
    uint16_t idMax;
    uint8_t  day;
    uint8_t  night;
  }};

  enum : uint8_t
  {{
{chr(10).join(enum_lines)}
  }};

{chr(10).join(arrays)}

  inline const Icon ICONS[] = {{
{chr(10).join(entries)}
  }};

  inline const Condition CONDITIONS[] = {{
{chr(10).join(condition_lines)}
  }};

  /**
   * Icona del codice condizione OpenWeather; night sceglie la variante
   * notturna, che esiste solo per sereno e poche nubi. Un codice fuori
   * tabella, compreso lo 0 di un dato mancante, ripiega su {spec['fallback']}.
   */
  inline const Icon& forCondition(uint16_t conditionId, bool night)
  {{
    for (const Condition& condition : CONDITIONS)
    {{
      if (conditionId >= condition.idMin && conditionId <= condition.idMax)
        return ICONS[night ? condition.night : condition.day];
    }}
    return ICONS[{spec['fallback'].upper()}];
  }}
}}

#endif
"""


def convert(spec, base_dir=ICONS_DIR, header_path=DEFAULT_HEADER_PATH, side=DEFAULT_SIDE):
    """Scrive icons_weather.h dalle icone base; il lato deve essere multiplo di 8."""
    if side % 8:
        raise ValueError(f"il lato {side} deve essere multiplo di 8")
    text = build_header(spec, icon_planes(spec, base_dir, side), side)
    Path(header_path).write_text(text, encoding="utf-8", newline="\n")
    return Path(header_path)


def main(argv=None):
    """Riga di comando: extract e convert."""
    parser = argparse.ArgumentParser(description="Icone meteo Material Symbols per il firmware.")
    commands = parser.add_subparsers(dest="command", required=True)
    extract_parser = commands.add_parser("extract", help="rende le icone base dal font")
    extract_parser.add_argument("--font", type=Path, default=DEFAULT_FONT_PATH)
    convert_parser = commands.add_parser("convert", help="scrive icons_weather.h dalle icone base")
    convert_parser.add_argument("--side", type=int, default=DEFAULT_SIDE)
    convert_parser.add_argument("--header", type=Path, default=DEFAULT_HEADER_PATH)
    args = parser.parse_args(argv)

    spec = load_spec()
    if args.command == "extract":
        for path in extract(spec, args.font):
            print(path.relative_to(PROJECT_DIR))
    else:
        print(convert(spec, side=args.side, header_path=args.header))
    return 0


if __name__ == "__main__":
    sys.exit(main())
