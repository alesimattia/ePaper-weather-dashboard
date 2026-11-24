Immagini sorgente del wallpaper del riquadro in alto a sinistra, mostrato fuori dalle fasce di percorso e cinema e quando la loro immagine non è disponibile.
Il wallpaper predefinito è img_great_wave.h, compilato nel firmware: arriva con l'upload dell'IDE o di VS Code. Ha gli stessi byte di great_wave_097c.bin.
Per sostituirlo senza ricompilare, tools/wallpaper_bin.py genera da un'immagine un <nome>_<pannello>.bin per pannello, che si carica dalla pagina /wallpaper della finestra di manutenzione nella partizione "wallpaper".
