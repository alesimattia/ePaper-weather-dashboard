"""
Genera il wallpaper del riquadro in alto a sinistra per ogni pannello, nel
formato che la pagina /wallpaper della finestra di manutenzione carica nella
partizione "wallpaper": i due piani 1bpp nero e rosso di /cinema/arduino, a
misura Layout::CINEMA_W x CINEMA_H del pannello.

Dithering e impacchettamento sono quelli della webapp (webapp/dithering.py,
webapp/arduino_format.py), così il wallpaper ha la stessa resa delle immagini
del cinema; le misure le legge tools/layout_preview.py dai Layout_*.h.

Scrive wallpaper/<nome>_<pannello>.bin, uno per pannello. Il firmware rifiuta
un file di misura diversa, e un file identico a quello già caricato non viene
riscritto.

Uso:
    python tools/wallpaper_bin.py wallpaper/great_wave.jpg [--name great_wave]

Dipendenze: numpy e Pillow, già nel venv di routefinder (routefinder/.venv).

@Mattia Alesi
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "webapp"))
sys.path.insert(0, str(ROOT / "tools"))

from arduino_format import encode  # noqa: E402
from dithering import quantize  # noqa: E402
from layout_preview import panel_names, read_constants  # noqa: E402

COLOR_MODE = "bwr"
DITHER = "floyd"


def render(source, width, height):
    """Ritaglia l'immagine al riquadro senza deformarla, la ditherizza nella palette
    a tre colori e restituisce i due piani concatenati."""
    fitted = ImageOps.fit(source.convert("RGB"), (width, height), method=Image.Resampling.LANCZOS)
    return encode(quantize(fitted, COLOR_MODE, DITHER), COLOR_MODE)


def main():
    parser = argparse.ArgumentParser(description="Wallpaper del riquadro, un .bin per pannello")
    parser.add_argument("image", type=Path, help="immagine sorgente (jpg, png, ...)")
    parser.add_argument("--name", help="nome dei file prodotti, di default quello dell'immagine")
    arguments = parser.parse_args()
    name = arguments.name or arguments.image.stem
    source = Image.open(arguments.image)
    for panel in panel_names():
        layout = read_constants(panel)
        body = render(source, layout["CINEMA_W"], layout["CINEMA_H"])
        if len(body) != layout["CINEMA_TOTAL_SZ"]:
            sys.exit(f"{panel}: {len(body)} byte invece di {layout['CINEMA_TOTAL_SZ']}")
        output = ROOT / "wallpaper" / f"{name}_{panel}.bin"
        output.write_bytes(body)
        print(f"scritto {output.relative_to(ROOT).as_posix()}: "
              f"{layout['CINEMA_W']} x {layout['CINEMA_H']} px, {len(body)} byte")


if __name__ == "__main__":
    main()
