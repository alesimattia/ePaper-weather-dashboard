"""
Ottiene un refresh token Google via OAuth e lo scrive dentro un header C di
configurazione, insieme a client id e client secret.

Serve per i dispositivi che non possono fare un login interattivo: il token si
emette una volta sola da questa macchina e poi vive nell'header, che il
dispositivo compila dentro il proprio firmware.

Uso:
    python3 google_oauth_setup.py

Nessun argomento e nessun percorso assoluto: lo script lavora sulla cartella in
cui e' posizionato. Li' cerca un client_secret_*.json (come viene scaricato dalla
pagina Credenziali di Google Cloud Console, tipo "Applicazione desktop"); se non
c'e', riusa client id e secret gia' presenti nell'header, cosi' dopo la prima
configurazione il JSON non e' piu' necessario. Apre il browser per
l'autorizzazione e aggiorna l'header sul posto. I valori non vengono stampati.

Per adattarlo a un altro progetto si toccano solo le costanti qui sotto.

Dipendenza: google-auth-oauthlib.
    python3 -m pip install google-auth-oauthlib
"""

import json
import os
import re
import sys
import tempfile
from pathlib import Path

# --- Configurazione -------------------------------------------------------
# Tutto e' relativo alla cartella dello script, cosi' spostarlo altrove lo fa
# lavorare sul progetto che lo ospita senza modifiche.
SCRIPT_DIR = Path(__file__).resolve().parent

# Header di configurazione da aggiornare, cercato accanto allo script.
ENV_FILENAME = "Env.h"

# Scope da autorizzare. Vanno chiesti tutti insieme: un refresh token vale solo
# per gli scope con cui e' stato emesso, e aggiungerne uno dopo richiede di
# rifare l'autorizzazione da capo.
SCOPES = [
    "https://www.googleapis.com/auth/calendar.readonly",
    "https://www.googleapis.com/auth/gmail.readonly",
]

# Macro da scrivere -> campo da cui prendere il valore. "refresh_token" arriva
# dal flusso OAuth, gli altri dalla credenziale (JSON scaricato o header).
MACRO = {
    "GOOGLE_CLIENT_ID":     "client_id",
    "GOOGLE_CLIENT_SECRET": "client_secret",
    "GOOGLE_REFRESH_TOKEN": "refresh_token",
}

# Valori del template che non contano come credenziale gia' configurata.
PLACEHOLDER_FRAMMENTI = ("paste_", "0000000")

# Endpoint usati quando la credenziale viene ricostruita dall'header invece che
# dal JSON: sono gli stessi che Google mette nel file scaricato.
AUTH_URI = "https://accounts.google.com/o/oauth2/auth"
TOKEN_URI = "https://oauth2.googleapis.com/token"
# --------------------------------------------------------------------------


def percorso_header():
    """Header di configurazione accanto allo script."""
    percorso = SCRIPT_DIR / ENV_FILENAME
    if not percorso.exists():
        sys.exit(f"{percorso} non esiste: crealo (di solito copiando il "
                 f"template accanto) e rilancia.")
    return percorso


def trova_client_secret():
    """Ritorna il client_secret_*.json della cartella dello script, o None se
    non c'e' (in quel caso la credenziale si legge dall'header). Con piu' di un
    file si ferma: sceglierne uno significherebbe usare il progetto Google
    sbagliato."""
    trovati = sorted(SCRIPT_DIR.glob("client_secret_*.json"))
    if len(trovati) > 1:
        elenco = "\n  ".join(p.name for p in trovati)
        sys.exit(f"Piu' di un client_secret_*.json in {SCRIPT_DIR}:\n  {elenco}\n"
                 "Tieni solo quello del progetto da usare.")
    return trovati[0] if trovati else None


def leggi_macro(testo, macro):
    """Ritorna il valore fra virgolette di un #define, o None se assente o se
    contiene ancora un placeholder del template."""
    m = re.search(r'^[ \t]*#define[ \t]+' + re.escape(macro) + r'[ \t]+"([^"]*)"',
                  testo, re.MULTILINE)
    if not m:
        return None
    valore = m.group(1)
    if not valore or any(f in valore for f in PLACEHOLDER_FRAMMENTI):
        return None
    return valore


def credenziale(percorso_json, testo_header):
    """Ritorna (client_id, client_secret) prendendoli dal JSON scaricato se
    presente, altrimenti dall'header gia' configurato.

    :param percorso_json: Path del client_secret_*.json, oppure None
    :param testo_header: contenuto dell'header di configurazione
    """
    if percorso_json is not None:
        print(f"Credenziali: {percorso_json.name}")
        with open(percorso_json, encoding="utf-8") as fh:
            dati = json.load(fh)
        # Le app desktop stanno sotto 'installed'; 'web' e' il tipo sbagliato
        # per questo flusso e non avrebbe il redirect su loopback.
        if "installed" not in dati:
            sys.exit("Il JSON non e' di un client OAuth 'Applicazione desktop'.\n"
                     "Ricrea la credenziale con quel tipo e riscaricala.")
        sezione = dati["installed"]
        mancanti = [c for c in ("client_id", "client_secret") if not sezione.get(c)]
        if mancanti:
            sys.exit("Campi assenti nel JSON: " + ", ".join(mancanti))
        return sezione["client_id"], sezione["client_secret"]

    # Nessun JSON: la credenziale e' gia' nell'header da una configurazione
    # precedente, serve solo riautorizzare per un token nuovo.
    macro_id = next(m for m, c in MACRO.items() if c == "client_id")
    macro_secret = next(m for m, c in MACRO.items() if c == "client_secret")
    client_id = leggi_macro(testo_header, macro_id)
    client_secret = leggi_macro(testo_header, macro_secret)
    if not client_id or not client_secret:
        sys.exit(
            f"Nessun client_secret_*.json in {SCRIPT_DIR} e {ENV_FILENAME} non ha\n"
            f"ancora {macro_id} / {macro_secret} valorizzati.\n"
            "Scarica il JSON da Google Cloud Console -> API e servizi -> Credenziali,\n"
            "riga dell'ID client OAuth di tipo 'Applicazione desktop' (icona di\n"
            "download), e lascialo in questa cartella senza rinominarlo."
        )
    print(f"Nessun JSON in cartella: riuso la credenziale gia' in {ENV_FILENAME}.")
    return client_id, client_secret


def aggiorna_header(percorso, valori):
    """Riscrive i #define indicati lasciando intatto tutto il resto del file:
    commenti, ordine delle righe e allineamento della colonna dei valori.
    Sostituisce solo il testo fra le virgolette, quindi funziona sia sui
    placeholder di un template sia su valori gia' presenti.

    La scrittura e' atomica (file temporaneo nella stessa cartella + replace):
    un'interruzione a meta' non lascia un header troncato, che impedirebbe la
    compilazione.

    :param valori: dict {nome macro: valore da scrivere}
    """
    testo = percorso.read_text(encoding="utf-8")
    mancanti = []

    for macro, valore in valori.items():
        # Cattura il prefisso con la sua spaziatura, cosi' la riga mantiene la
        # formattazione che ha nel file.
        pattern = re.compile(r'^([ \t]*#define[ \t]+' + re.escape(macro) + r'[ \t]+)"[^"]*"',
                             re.MULTILINE)
        # Replacement come funzione: un valore che contenesse \1 o simili non
        # verrebbe interpretato come riferimento a un gruppo.
        testo, n = pattern.subn(lambda m: m.group(1) + '"' + valore + '"', testo)
        if n == 0:
            mancanti.append(macro)

    if mancanti:
        sys.exit(f"Queste macro non sono state trovate in {percorso.name}: "
                 + ", ".join(mancanti) + "\nAggiungile e rilancia.")

    fd, tmp = tempfile.mkstemp(dir=str(SCRIPT_DIR), prefix=f".{ENV_FILENAME}.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(testo)
        os.replace(tmp, percorso)
    except BaseException:
        os.unlink(tmp)
        raise


def main():
    try:
        from google_auth_oauthlib.flow import InstalledAppFlow
    except ImportError:
        sys.exit("Manca google-auth-oauthlib. Installalo con:\n"
                 f"  {sys.executable} -m pip install google-auth-oauthlib")

    header = percorso_header()
    client_id, client_secret = credenziale(trova_client_secret(),
                                           header.read_text(encoding="utf-8"))

    # from_client_config accetta la stessa struttura del file scaricato, quindi
    # le due provenienze convergono qui su un solo percorso di codice.
    config = {"installed": {
        "client_id": client_id,
        "client_secret": client_secret,
        "auth_uri": AUTH_URI,
        "token_uri": TOKEN_URI,
    }}

    # prompt='consent' e' obbligatorio: senza, a un progetto gia' autorizzato
    # Google restituisce solo l'access token e il campo refresh resta vuoto.
    # access_type='offline' e' cio' che rende il refresh token emettibile.
    flow = InstalledAppFlow.from_client_config(config, SCOPES)
    print("Si apre il browser per l'autorizzazione...")
    cred = flow.run_local_server(port=0, prompt="consent", access_type="offline")

    if not cred.refresh_token:
        sys.exit("Google non ha restituito un refresh token. Riprova: di solito\n"
                 "succede quando il consenso era gia' attivo e prompt=consent non\n"
                 "e' stato applicato.")

    sorgente = {
        "client_id": client_id,
        "client_secret": client_secret,
        "refresh_token": cred.refresh_token,
    }
    aggiorna_header(header, {m: sorgente[c] for m, c in MACRO.items()})

    print(f"\n{header.name} aggiornato: " + ", ".join(MACRO) +
          f"\nScope autorizzati: {', '.join(SCOPES)}")


if __name__ == "__main__":
    main()
