"""
Convertitore immagini per e-paper display (GxEPD2 / SOLUM 672x960 3-colori).

Interfaccia grafica semplice per convertire PNG/JPG/WEBP/BMP/GIF in array
PROGMEM C++ pronti da includere in uno sketch Arduino/ESP32.

Output: file 'img_<nomeoriginale>.h' nella stessa cartella dello script,
dove <nomeoriginale> è lo stem dell'immagine sorgente sanitizzato per C
(caratteri non alfanumerici sostituiti con underscore).

Modalita' colore supportate (in ordine nella UI):
    - B/N (2 colori)                -> 1 array 1bpp: img_<nome>
    - B/N + Rosso (3 colori)        -> 2 array 1bpp: img_<nome>_black, _red
    - B/N + Rosso + Giallo (4 col.) -> 3 array 1bpp: img_<nome>_black, _red, _yellow
    - RGB565 16bpp                  -> 1 array uint16_t: img_<nome>

Convenzione di packing bit (per i canali 1bpp): MSB-first, 1 bpp. Per ciascun
canale il bit vale 1 quando il pixel NON appartiene a quel colore (compatibile
con l'input previsto dai metodi writeImage(black, color) dei driver GxEPD2).

Dithering disponibili: Floyd-Steinberg, Atkinson, Bayer 8x8, nessuno (soglia).
In RGB565 selezionando un dithering l'immagine viene prima quantizzata alla
palette BWRY (4 colori) con l'algoritmo scelto e poi ricodificata in RGB565:
il risultato è un'immagine a colori saturi con pattern di dithering che il
driver on-device rimappera' in modo pulito sulla propria palette nativa.

Drag-and-drop: trascina un file immagine sulla finestra per caricarlo
automaticamente (richiede la libreria tkinterdnd2; se assente resta
disponibile il pulsante "Sfoglia...").

Dipendenze: Pillow, NumPy, tkinterdnd2 (opzionale per drag-and-drop).
"""

import os
import re
import sys
from datetime import datetime
from pathlib import Path
from urllib.parse import unquote, urlparse
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

try:
    from PIL import Image, ImageOps, ImageTk
    import numpy as np
except ImportError as e:
    # Mostra errore leggibile senza console (script .pyw)
    root = tk.Tk()
    root.withdraw()
    messagebox.showerror(
        "Dipendenze mancanti",
        f"Impossibile importare {e.name}.\n\n"
        "Installa le dipendenze con:\n"
        "  pip install Pillow numpy tkinterdnd2"
    )
    sys.exit(1)

# tkinterdnd2 è opzionale: se assente il drag-and-drop viene disabilitato
# ma lo script resta utilizzabile con il pulsante "Sfoglia...".
try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
    DND_AVAILABLE = True
except ImportError:
    TkinterDnD = None
    DND_FILES = None
    DND_AVAILABLE = False


SCRIPT_DIR = Path(__file__).resolve().parent

# Preset (etichetta, larghezza, altezza, modalita' colore preferita).
# La modalita' colore viene applicata automaticamente quando si seleziona il
# preset: None = non modificare la scelta corrente dell'utente.
SIZE_PRESETS = [
    ("SOLUM 672x960 (landscape)", 960, 672, "bwr"),
    ("SOLUM 672x960 (portrait)", 672, 960, "bwr"),
    ("GDEY0420F51 400x300 (4 colori nativi)", 400, 300, "bwry"),
    ("Waveshare 4.2\" 400x300", 400, 300, None),
    ("Waveshare 7.5\" 800x480", 800, 480, None),
    ("Personalizzato", None, None, None),
]

FIT_MODES = [
    ("Crop centrato (mantiene aspect)", "crop"),
    ("Stretch (deforma all'aspect)", "stretch"),
    ("Letterbox (padding bianco)", "letterbox"),
]

DITHER_MODES = [
    ("Floyd-Steinberg", "fs"),
    ("Atkinson", "atkinson"),
    ("Bayer 8x8 (ordered)", "bayer"),
    ("Nessuno (soglia)", "none"),
]

# Ordine esplicito: B/N -> B/N+R -> B/N+R+Y -> RGB565.
COLOR_MODES = [
    ("B/N (2 colori)", "bw"),
    ("B/N + Rosso (3 colori)", "bwr"),
    ("B/N + Rosso + Giallo (4 colori)", "bwry"),
    ("RGB565 16bpp (con dithering opzionale)", "rgb565"),
]

# Palette RGB per ogni modalita' a palette (indice 0 = bianco, sempre).
PALETTES = {
    "bw":   [(255, 255, 255), (0, 0, 0)],
    "bwr":  [(255, 255, 255), (0, 0, 0), (255, 0, 0)],
    "bwry": [(255, 255, 255), (0, 0, 0), (255, 0, 0), (255, 255, 0)],
}

# Matrice di Bayer 8x8 per il dithering ordered. Valori 0..63.
BAYER_8X8 = np.array([
    [ 0, 32,  8, 40,  2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44,  4, 36, 14, 46,  6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [ 3, 35, 11, 43,  1, 33,  9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47,  7, 39, 13, 45,  5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float32)


def load_palette_image(palette_rgb):
    """Crea un'immagine di palette PIL dai colori indicati.

    Pillow richiede una palette con 256 entry, le posizioni non usate
    vengono riempite con nero.
    """
    palette_data = []
    for rgb in palette_rgb:
        palette_data.extend(rgb)
    while len(palette_data) < 768:
        palette_data.extend((0, 0, 0))

    pal = Image.new("P", (1, 1))
    pal.putpalette(palette_data)
    return pal


def fit_image(img, target_w, target_h, mode):
    """Adatta l'immagine alle dimensioni target secondo la modalita' scelta."""
    img = img.convert("RGB")
    if mode == "stretch":
        return img.resize((target_w, target_h), Image.LANCZOS)
    if mode == "crop":
        return ImageOps.fit(img, (target_w, target_h),
                            method=Image.LANCZOS, centering=(0.5, 0.5))
    if mode == "letterbox":
        return ImageOps.pad(img, (target_w, target_h),
                            method=Image.LANCZOS, color=(255, 255, 255))
    raise ValueError(f"Modalita' fit sconosciuta: {mode}")


def quantize_floyd_steinberg(img_rgb, palette_rgb):
    """Quantizza con dithering Floyd-Steinberg (nativo Pillow)."""
    pal_img = load_palette_image(palette_rgb)
    quantized = img_rgb.quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)
    return np.array(quantized, dtype=np.uint8)


def quantize_threshold(img_rgb, palette_rgb):
    """Quantizzazione per soglia (nearest neighbor RGB, nessun dither)."""
    pal_img = load_palette_image(palette_rgb)
    quantized = img_rgb.quantize(palette=pal_img, dither=Image.Dither.NONE)
    return np.array(quantized, dtype=np.uint8)


def quantize_bayer(img_rgb, palette_rgb):
    """Dithering ordered su matrice di Bayer 8x8.

    Per ciascun pixel aggiunge un offset RGB proporzionale al valore della
    matrice di Bayer (centrato in zero) e poi assegna il colore di palette
    piu' vicino. Produce un pattern regolare stabile nel tempo, adatto a
    immagini con gradienti o sfondi uniformi.
    """
    arr = np.array(img_rgb, dtype=np.float32)
    h, w, _ = arr.shape
    # Bayer normalizzata in [-0.5, 0.5] e poi scalata in ampiezza di colore.
    bayer_norm = (BAYER_8X8 + 0.5) / 64.0 - 0.5
    tiles_y = (h + 7) // 8
    tiles_x = (w + 7) // 8
    bayer_tiled = np.tile(bayer_norm, (tiles_y, tiles_x))[:h, :w]
    # Ampiezza scelta empiricamente: ~64/255 di colore. Maggiore = piu' rumore.
    amplitude = 64.0
    perturbed = arr + (bayer_tiled[:, :, None] * amplitude)

    palette = np.array(palette_rgb, dtype=np.float32)  # (N, 3)
    flat = perturbed.reshape(-1, 3)
    # Distanze al quadrato: (H*W, N)
    diffs = flat[:, None, :] - palette[None, :, :]
    dists = np.sum(diffs * diffs, axis=2)
    idx = np.argmin(dists, axis=1).astype(np.uint8)
    return idx.reshape(h, w)


def quantize_atkinson(img_rgb, palette_rgb):
    """Dithering Atkinson: distribuisce 6/8 dell'errore ai 6 vicini.

    Pattern di distribuzione (X = pixel corrente):
                  X  1/8  1/8
        1/8  1/8  1/8
             1/8

    Implementazione in pure-Python sulle nested list per evitare l'overhead
    numpy per-pixel (diventerebbe lentissimo su 960x672).
    """
    arr_np = np.array(img_rgb, dtype=np.float32)
    h, w, _ = arr_np.shape
    arr = arr_np.tolist()  # nested list float - iterazione scalare veloce
    palette = [(float(r), float(g), float(b)) for r, g, b in palette_rgb]
    result = [[0] * w for _ in range(h)]

    for y in range(h):
        row = arr[y]
        row_n1 = arr[y + 1] if y + 1 < h else None
        row_n2 = arr[y + 2] if y + 2 < h else None
        result_row = result[y]
        for x in range(w):
            p = row[x]
            or_ = p[0]; og = p[1]; ob = p[2]
            # Nearest neighbor su palette piccola (2-4 colori).
            best_i = 0
            best_d = 1e18
            for i, (pr, pg, pb) in enumerate(palette):
                dr = or_ - pr
                dg = og - pg
                db = ob - pb
                d = dr * dr + dg * dg + db * db
                if d < best_d:
                    best_d = d
                    best_i = i
            pr, pg, pb = palette[best_i]
            result_row[x] = best_i
            er = (or_ - pr) * 0.125
            eg = (og - pg) * 0.125
            eb = (ob - pb) * 0.125
            if x + 1 < w:
                q = row[x + 1]; q[0] += er; q[1] += eg; q[2] += eb
            if x + 2 < w:
                q = row[x + 2]; q[0] += er; q[1] += eg; q[2] += eb
            if row_n1 is not None:
                if x - 1 >= 0:
                    q = row_n1[x - 1]; q[0] += er; q[1] += eg; q[2] += eb
                q = row_n1[x]; q[0] += er; q[1] += eg; q[2] += eb
                if x + 1 < w:
                    q = row_n1[x + 1]; q[0] += er; q[1] += eg; q[2] += eb
            if row_n2 is not None:
                q = row_n2[x]; q[0] += er; q[1] += eg; q[2] += eb

    return np.array(result, dtype=np.uint8)


QUANTIZERS = {
    "fs": quantize_floyd_steinberg,
    "atkinson": quantize_atkinson,
    "bayer": quantize_bayer,
    "none": quantize_threshold,
}


def rgb_to_rgb565(img_rgb):
    """Converte un'immagine RGB in un array 2D di uint16 in formato RGB565."""
    arr = np.array(img_rgb.convert("RGB"), dtype=np.uint8)
    r = (arr[:, :, 0].astype(np.uint16) >> 3) & 0x1F
    g = (arr[:, :, 1].astype(np.uint16) >> 2) & 0x3F
    b = (arr[:, :, 2].astype(np.uint16) >> 3) & 0x1F
    return ((r << 11) | (g << 5) | b).astype(np.uint16)


def pack_mask_msb(mask):
    """Impacca una mask booleana 2D in bytes MSB-first, riga per riga.

    Le righe vengono paddate a multiplo di 8 pixel come richiesto dai driver
    GxEPD2. mask[y, x] True -> bit 1, False -> bit 0.
    """
    h, w = mask.shape
    pad_w = (w + 7) // 8 * 8
    if pad_w != w:
        padded = np.ones((h, pad_w), dtype=bool)  # padding = bit 1 (bianco/non-color)
        padded[:, :w] = mask
        mask = padded
    reshaped = mask.reshape(h, pad_w // 8, 8)
    weights = np.array([128, 64, 32, 16, 8, 4, 2, 1], dtype=np.uint8)
    packed = (reshaped.astype(np.uint8) * weights).sum(axis=2).astype(np.uint8)
    return packed.flatten().tobytes()


def indices_to_channel_masks(indices, color_mode):
    """Dagli indici di palette costruisce le mask per ciascun canale.

    Convenzione (compatibile con GxEPD2 writeImage(black, color)):
        mask canale = True dove il pixel NON ha quel colore (bit 1 = pixel non-in-canale).
    """
    masks = {}
    if color_mode == "bw":
        # Solo canale nero: True dove il pixel è bianco (indice 0).
        masks["black"] = (indices == 0)
    elif color_mode == "bwr":
        masks["black"] = (indices != 1)
        masks["red"] = (indices != 2)
    elif color_mode == "bwry":
        masks["black"] = (indices != 1)
        masks["red"] = (indices != 2)
        masks["yellow"] = (indices != 3)
    else:
        raise ValueError(f"Modalita' colore sconosciuta: {color_mode}")
    return masks


def sanitize_identifier(stem):
    """Trasforma il nome file in identificatore C valido.

    Sostituisce caratteri non alfanumerici con underscore, collassa underscore
    multipli e si assicura che il nome non inizi con una cifra. Il prefisso
    'img_' viene aggiunto dal chiamante, quindi un primo carattere numerico
    è comunque gestito automaticamente.
    """
    cleaned = re.sub(r"[^a-zA-Z0-9_]", "_", stem)
    cleaned = re.sub(r"_+", "_", cleaned).strip("_")
    if not cleaned:
        cleaned = "image"
    return cleaned


def format_c_array_u8(var_name, data_bytes, per_line=16):
    """Formatta byte come array C PROGMEM di unsigned char."""
    lines = [f"const unsigned char {var_name}[] PROGMEM = {{"]
    for i in range(0, len(data_bytes), per_line):
        chunk = data_bytes[i:i + per_line]
        hex_bytes = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"\t{hex_bytes},")
    lines.append("};")
    return "\n".join(lines)


def format_c_array_u16(var_name, data_words, per_line=12):
    """Formatta word a 16 bit come array C PROGMEM di uint16_t (per RGB565)."""
    flat = list(data_words)
    lines = [f"const uint16_t {var_name}[] PROGMEM = {{"]
    for i in range(0, len(flat), per_line):
        chunk = flat[i:i + per_line]
        hex_words = ", ".join(f"0x{w:04X}" for w in chunk)
        lines.append(f"\t{hex_words},")
    lines.append("};")
    return "\n".join(lines)


def render_to_preview_image(result, color_mode, max_side=320):
    """Ricostruisce un'immagine RGB per anteprima.

    Per le modalita' a palette usa gli indici + palette; per RGB565 decodifica
    i 16 bit in RGB888 approssimato.
    """
    if color_mode == "rgb565":
        words = result  # np.uint16 HxW
        r5 = (words >> 11) & 0x1F
        g6 = (words >> 5)  & 0x3F
        b5 =  words        & 0x1F
        rgb = np.stack([
            ((r5 * 255) // 31).astype(np.uint8),
            ((g6 * 255) // 63).astype(np.uint8),
            ((b5 * 255) // 31).astype(np.uint8),
        ], axis=2)
        img = Image.fromarray(rgb, mode="RGB")
    else:
        # bw, bwr, bwry: tutti hanno palette negli indici 0..N.
        indices = result
        palette = np.array(PALETTES[color_mode], dtype=np.uint8)
        rgb = palette[indices]
        img = Image.fromarray(rgb, mode="RGB")
    img.thumbnail((max_side, max_side), Image.NEAREST)
    return img


class ConverterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Convertitore immagini e-paper")
        self.root.resizable(False, False)

        self.input_path = tk.StringVar(value="")
        self.preset_var = tk.StringVar(value=SIZE_PRESETS[0][0])
        self.width_var = tk.IntVar(value=960)
        self.height_var = tk.IntVar(value=672)
        self.fit_var = tk.StringVar(value="crop")
        self.dither_var = tk.StringVar(value="fs")
        self.color_var = tk.StringVar(value="bw")
        status_extra = " Trascina un'immagine qui per iniziare." if DND_AVAILABLE else ""
        self.status_var = tk.StringVar(
            value=f"Pronto. Seleziona un'immagine per iniziare.{status_extra}"
        )

        self._preview_ref = None   # evita garbage-collect della PhotoImage
        self._preview_after_id = None  # id del job after() di debounce

        self._build_ui()
        self._setup_drag_and_drop()
        self._setup_auto_preview_traces()

    def _build_ui(self):
        pad = {"padx": 8, "pady": 4}

        frm = ttk.Frame(self.root, padding=10)
        frm.grid(row=0, column=0, sticky="nsew")
        self._main_frame = frm  # per drop target

        # --- File input ---
        ttk.Label(frm, text="Immagine sorgente:").grid(row=0, column=0, sticky="w", **pad)
        self._path_entry = ttk.Entry(frm, textvariable=self.input_path, width=42)
        self._path_entry.grid(row=0, column=1, **pad)
        ttk.Button(frm, text="Sfoglia...", command=self._browse).grid(row=0, column=2, **pad)

        # --- Preset dimensioni ---
        ttk.Label(frm, text="Preset dimensioni:").grid(row=1, column=0, sticky="w", **pad)
        preset_cb = ttk.Combobox(
            frm, textvariable=self.preset_var, state="readonly",
            values=[p[0] for p in SIZE_PRESETS], width=40
        )
        preset_cb.grid(row=1, column=1, columnspan=2, sticky="w", **pad)
        preset_cb.bind("<<ComboboxSelected>>", self._on_preset_change)

        # --- Dimensioni manuali ---
        dim_frame = ttk.Frame(frm)
        dim_frame.grid(row=2, column=0, columnspan=3, sticky="w", **pad)
        ttk.Label(dim_frame, text="Larghezza:").grid(row=0, column=0, padx=(0, 4))
        ttk.Spinbox(dim_frame, from_=8, to=4096, increment=8,
                    textvariable=self.width_var, width=8).grid(row=0, column=1, padx=(0, 16))
        ttk.Label(dim_frame, text="Altezza:").grid(row=0, column=2, padx=(0, 4))
        ttk.Spinbox(dim_frame, from_=8, to=4096, increment=8,
                    textvariable=self.height_var, width=8).grid(row=0, column=3)

        # --- Adattamento ---
        self._radio_group(frm, "Adattamento:", FIT_MODES, self.fit_var, row=3)

        # --- Modalita' colore ---
        self._radio_group(frm, "Modalita' colore:", COLOR_MODES, self.color_var, row=4)

        # --- Dithering (sempre attivo: in RGB565 quantizza a BWRY prima
        # di ricodificare a 16 bit) ---
        self._radio_group(frm, "Dithering:", DITHER_MODES, self.dither_var, row=5)

        # --- Preview ---
        self.preview_label = ttk.Label(frm, text="(anteprima)", relief="sunken",
                                       anchor="center", width=40)
        self.preview_label.grid(row=6, column=0, columnspan=3, pady=8, sticky="ew")

        # --- Azioni (solo salvataggio: l'anteprima si aggiorna da sola) ---
        actions = ttk.Frame(frm)
        actions.grid(row=7, column=0, columnspan=3, pady=(4, 0), sticky="ew")
        ttk.Button(actions, text="Converti e salva .h",
                   command=self._convert_and_save).pack(side="right", padx=4)

        # --- Stato ---
        ttk.Label(frm, textvariable=self.status_var, foreground="#444",
                  wraplength=460, justify="left").grid(
            row=8, column=0, columnspan=3, sticky="w", pady=(8, 0))

    def _radio_group(self, parent, label, choices, var, row):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="nw", padx=8, pady=4)
        box = ttk.Frame(parent)
        box.grid(row=row, column=1, columnspan=2, sticky="w", padx=8, pady=4)
        for i, (txt, val) in enumerate(choices):
            ttk.Radiobutton(box, text=txt, variable=var, value=val).grid(
                row=i, column=0, sticky="w"
            )

    def _setup_drag_and_drop(self):
        """Registra il drop target sull'intera finestra se tkinterdnd2 presente."""
        if not DND_AVAILABLE:
            return
        try:
            self.root.drop_target_register(DND_FILES)
            self.root.dnd_bind("<<Drop>>", self._on_drop)
            self._main_frame.drop_target_register(DND_FILES)
            self._main_frame.dnd_bind("<<Drop>>", self._on_drop)
        except Exception:
            # Se la registrazione fallisce (runtime DnD mancante) proseguiamo
            # senza blocchi: resta disponibile il pulsante "Sfoglia...".
            pass

    def _setup_auto_preview_traces(self):
        """Registra trace su tutte le StringVar/IntVar che influenzano il risultato.

        Ogni modifica schedula un aggiornamento anteprima con debounce: la
        funzione vera parte solo dopo ~200 ms di quiete, evitando render a
        raffica mentre l'utente digita nei spinbox o cambia rapidamente
        radiobutton.
        """
        vars_to_watch = (
            self.input_path, self.width_var, self.height_var,
            self.fit_var, self.dither_var, self.color_var,
        )
        for v in vars_to_watch:
            v.trace_add("write", self._schedule_preview)

    def _schedule_preview(self, *_args):
        """Debounce: cancella l'after in volo e riparte 200 ms dopo."""
        if self._preview_after_id is not None:
            try:
                self.root.after_cancel(self._preview_after_id)
            except Exception:
                pass
        self._preview_after_id = self.root.after(200, self._do_preview)

    def _compute_preview(self):
        """Pipeline di quantizzazione ridotta per l'anteprima.

        Esegue fit alle dimensioni target e poi thumbnail a max 320 px di
        lato: la quantizzazione lavora su un'immagine piccola (velocissima
        anche per Atkinson). Restituisce (result_array, color_mode) oppure
        None se non c'è nulla da mostrare.
        """
        path = self.input_path.get().strip()
        if not path or not os.path.isfile(path):
            return None
        try:
            tw = int(self.width_var.get())
            th = int(self.height_var.get())
        except (tk.TclError, ValueError):
            return None
        if tw <= 0 or th <= 0:
            return None

        img = Image.open(path)
        img = fit_image(img, tw, th, self.fit_var.get())
        # Downscale per render veloce in anteprima (mantiene aspect ratio).
        img = img.copy()
        img.thumbnail((320, 320), Image.LANCZOS)

        color_mode = self.color_var.get()
        dither_mode = self.dither_var.get()

        if color_mode == "rgb565":
            if dither_mode == "none":
                return rgb_to_rgb565(img), color_mode
            palette_rgb = PALETTES["bwry"]
            indices = QUANTIZERS[dither_mode](img, palette_rgb)
            pal_arr = np.array(palette_rgb, dtype=np.uint8)
            reduced = pal_arr[indices]
            return rgb_to_rgb565(Image.fromarray(reduced, "RGB")), color_mode

        palette_rgb = PALETTES[color_mode]
        indices = QUANTIZERS[dither_mode](img, palette_rgb)
        return indices, color_mode

    def _do_preview(self):
        """Calcola e disegna l'anteprima (chiamato dal debounce)."""
        self._preview_after_id = None
        try:
            result = self._compute_preview()
            if result is None:
                self.preview_label.configure(image="", text="(anteprima)")
                return
            data, color_mode = result
            prev_img = render_to_preview_image(data, color_mode)
            self._preview_ref = ImageTk.PhotoImage(prev_img)
            self.preview_label.configure(image=self._preview_ref, text="")
            h, w = data.shape
            n_colors = ("RGB565" if color_mode == "rgb565"
                        else f"{len(PALETTES[color_mode])} colori")
            src_name = Path(self.input_path.get()).name
            self.status_var.set(
                f"Anteprima: {src_name} - {w}x{h} - {n_colors} - "
                f"{self.color_var.get()}/{self.dither_var.get()}"
            )
        except Exception as ex:
            self.status_var.set(f"Errore anteprima: {ex}")
            self.preview_label.configure(image="", text="(errore)")

    def _on_drop(self, event):
        raw = event.data
        # Windows: path con spazi racchiusi in graffe. macOS: tkdnd restituisce
        # spesso URI 'file://...' con percent-encoding. Linux: path separati da
        # spazi o lista di URI. _parse_drop_paths gestisce tutti i casi.
        files = self._parse_drop_paths(raw)
        if not files:
            return
        path = files[0]
        if not os.path.isfile(path):
            self.status_var.set(f"Percorso non valido dal drop: {path}")
            return
        self.input_path.set(path)
        self.status_var.set(f"Caricata (drag&drop): {Path(path).name}")

    @staticmethod
    def _normalize_drop_path(token):
        """Converte un token di drop in percorso filesystem.

        Gestisce URI 'file://...' (macOS/Linux moderni) decodificando il
        percent-encoding; restituisce il token originale se gia' un path.
        """
        if not token:
            return token
        if token.startswith("file://"):
            parsed = urlparse(token)
            path = unquote(parsed.path)
            # Su Windows tkdnd potrebbe produrre 'file:///C:/...': rimuove lo
            # slash iniziale se seguito da 'lettera:'.
            if (len(path) >= 3 and path[0] == "/"
                    and path[2] == ":" and path[1].isalpha()):
                path = path[1:]
            return path
        # Path URL-encoded senza schema (raro ma possibile su alcuni DE Linux).
        if "%" in token and "/" in token:
            return unquote(token)
        return token

    @staticmethod
    def _parse_drop_paths(raw):
        """Estrae i percorsi file da una stringa di drop tkinterdnd2."""
        out = []
        token = []
        in_brace = False
        for ch in raw:
            if ch == "{":
                in_brace = True
            elif ch == "}":
                in_brace = False
                out.append("".join(token))
                token = []
            elif ch == " " and not in_brace:
                if token:
                    out.append("".join(token))
                    token = []
            else:
                token.append(ch)
        if token:
            out.append("".join(token))
        return [ConverterApp._normalize_drop_path(p) for p in out if p]

    def _on_preset_change(self, _event=None):
        sel = self.preset_var.get()
        for name, w, h, color_mode in SIZE_PRESETS:
            if name != sel:
                continue
            if w is not None:
                self.width_var.set(w)
                self.height_var.set(h)
            if color_mode is not None:
                self.color_var.set(color_mode)
            break

    def _browse(self):
        path = filedialog.askopenfilename(
            title="Scegli un'immagine",
            filetypes=[
                ("Immagini", "*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tiff *.tif"),
                ("Tutti i file", "*.*"),
            ],
        )
        if path:
            self.input_path.set(path)
            self.status_var.set(f"Caricata: {Path(path).name}")

    def _load_and_process(self):
        """Carica, ridimensiona e quantizza. Restituisce (result, color_mode)
        dove 'result' è un np.ndarray di indici di palette per le modalita'
        1bpp oppure un np.ndarray uint16 RGB565 per la modalita' 'rgb565'.
        """
        path = self.input_path.get().strip()
        if not path:
            raise ValueError("Nessuna immagine selezionata.")
        if not os.path.exists(path):
            raise FileNotFoundError(f"File non trovato: {path}")

        w = int(self.width_var.get())
        h = int(self.height_var.get())
        if w <= 0 or h <= 0:
            raise ValueError("Dimensioni non valide.")

        img = Image.open(path)
        img = fit_image(img, w, h, self.fit_var.get())

        color_mode = self.color_var.get()
        dither_mode = self.dither_var.get()

        if color_mode == "rgb565":
            if dither_mode == "none":
                # RGB565 diretto: conserva tutti i colori dell'immagine.
                return rgb_to_rgb565(img), color_mode
            # RGB565 + dithering: quantizziamo a palette BWRY con l'algoritmo
            # scelto, poi ricodifichiamo quei 4 colori saturi in RGB565. Cosi'
            # il driver on-device avra' un'immagine gia' "pulita" da quantizzare
            # sui propri 3-4 colori nativi, con pattern di dithering coerente.
            palette_rgb = PALETTES["bwry"]
            indices = QUANTIZERS[dither_mode](img, palette_rgb)
            pal_arr = np.array(palette_rgb, dtype=np.uint8)
            reduced_rgb = pal_arr[indices]
            return rgb_to_rgb565(Image.fromarray(reduced_rgb, "RGB")), color_mode

        # Tutte le modalita' a palette (bw, bwr, bwry) usano
        # la stessa pipeline di quantizzazione.
        palette_rgb = PALETTES[color_mode]
        quantizer = QUANTIZERS[dither_mode]
        indices = quantizer(img, palette_rgb)
        return indices, color_mode

    def _convert_and_save(self):
        try:
            self.status_var.set("Conversione in corso...")
            self.root.update_idletasks()
            result, color_mode = self._load_and_process()

            src_stem = Path(self.input_path.get()).stem
            base_name = f"img_{sanitize_identifier(src_stem)}"
            out_path = SCRIPT_DIR / f"{base_name}.h"

            if out_path.exists():
                overwrite = messagebox.askyesno(
                    "File esistente",
                    f"Il file '{out_path.name}' esiste gia'.\n\nSovrascrivere?"
                )
                if not overwrite:
                    self.status_var.set("Operazione annullata dall'utente.")
                    return

            h, w = result.shape
            dither_label = "-" if color_mode == "rgb565" else self.dither_var.get()
            header_guard = base_name.upper() + "_H"
            lines = [
                f"// {out_path.name} - generato da epd_image_converter.pyw",
                f"// Sorgente: {Path(self.input_path.get()).name}",
                f"// Dimensioni: {w}x{h} - Modalita': {color_mode.upper()} - "
                f"Dithering: {dither_label} - Fit: {self.fit_var.get()}",
                f"// Generato: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
                "//",
                "// NOTA: questo file usa GxEPDImage::Descriptor. Includilo DOPO",
                "// l'header del driver (es. \"GxEPD2_097c_SOLUM_672x960.h\") che",
                "// definisce il namespace GxEPDImage. Se il tipo non è presente",
                "// puoi comunque usare gli array raw direttamente.",
                "",
                f"#ifndef {header_guard}",
                f"#define {header_guard}",
                "",
                "#if defined(ESP8266) || defined(ESP32)",
                "#include <pgmspace.h>",
                "#else",
                "#include <avr/pgmspace.h>",
                "#endif",
                "",
            ]

            # Determina layout canali e formato descrittore.
            # Prima calcola gli array (senza scriverli), poi scrive descriptor,
            # poi scrive gli array — il descriptor appare per primo nel file.
            channels_order = []
            descriptor_name = f"{base_name}_desc"
            array_lines = []  # stringhe pronte da appendere dopo il descriptor

            if color_mode == "bw":
                fmt_enum = "FORMAT_BW_1BPP"
                data0_ref = base_name
                data1_ref = "nullptr"
                data2_ref = "nullptr"
                fwd_decls = [f"extern const unsigned char {base_name}[];"]

                packed = pack_mask_msb(indices_to_channel_masks(result, "bw")["black"])
                array_lines.append(format_c_array_u8(base_name, packed))
                array_lines.append("")

            elif color_mode in ("bwr", "bwry"):
                if color_mode == "bwr":
                    channels_order = ["black", "red"]
                    fmt_enum = "FORMAT_BWR_1BPP"
                else:
                    channels_order = ["black", "red", "yellow"]
                    fmt_enum = "FORMAT_BWRY_1BPP"
                data0_ref = f"{base_name}_black"
                data1_ref = f"{base_name}_red"
                data2_ref = f"{base_name}_yellow" if color_mode == "bwry" else "nullptr"
                fwd_decls = [
                    f"extern const unsigned char {base_name}_{ch}[];"
                    for ch in channels_order
                ]

                masks = indices_to_channel_masks(result, color_mode)
                for channel in channels_order:
                    var = f"{base_name}_{channel}"
                    array_lines.append(format_c_array_u8(var, pack_mask_msb(masks[channel])))
                    array_lines.append("")

            elif color_mode == "rgb565":
                fmt_enum = "FORMAT_RGB565"
                data0_ref = base_name
                data1_ref = "nullptr"
                data2_ref = "nullptr"
                fwd_decls = [f"extern const uint16_t {base_name}[];"]

                words = result.flatten().tolist()
                array_lines.append(format_c_array_u16(base_name, words))
                array_lines.append("")

            else:
                raise ValueError(f"Modalita' colore non supportata: {color_mode}")

            # Descrittore unificato prima degli array.
            # Forward declarations necessarie perché gli array sono definiti dopo.
            lines.extend([
                "#ifdef _GxEPD2_097c_SOLUM_672x960_H_",
                *fwd_decls,
                "",
                f"const GxEPDImage::Descriptor {descriptor_name} = {{",
                f"\tGxEPDImage::{fmt_enum},",
                f"\t{w}, {h},",
                f"\t(const uint8_t*){data0_ref},",
                f"\t(const uint8_t*){data1_ref},",
                f"\t(const uint8_t*){data2_ref}",
                "};",
                "#endif",
                "",
            ])

            lines.extend(array_lines)

            lines.append(f"#endif // {header_guard}")
            lines.append("")

            out_path.write_text("\n".join(lines), encoding="utf-8")

            size_kb = out_path.stat().st_size / 1024
            self.status_var.set(
                f"Salvato: {out_path.name} ({size_kb:.1f} KB) in {SCRIPT_DIR}"
            )

            if channels_order:
                canali_txt = f"\nCanali: {', '.join(base_name + '_' + c for c in channels_order)}"
            else:
                canali_txt = ""

            messagebox.showinfo(
                "Conversione completata",
                f"File generato:\n{out_path}\n\n"
                f"Array raw: {base_name}"
                + canali_txt
                + f"\nDescrittore unificato: {descriptor_name}"
                + f"\n\nUso nello sketch:\n  display.epd2.showImage({descriptor_name});"
            )
        except Exception as ex:
            messagebox.showerror("Errore conversione", str(ex))
            self.status_var.set(f"Errore: {ex}")


def main():
    # Usa TkinterDnD.Tk() se disponibile (abilita drag-and-drop nativo
    # dalla shell dell'OS), altrimenti fallback a tk.Tk() standard.
    root = TkinterDnD.Tk() if DND_AVAILABLE else tk.Tk()
    try:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
    except tk.TclError:
        pass
    ConverterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
