"""
Ottiene un refresh token Microsoft Graph via device code flow e lo scrive dentro
un header C di configurazione, insieme al client id.

Gemello di google_oauth_setup.py, per il ramo Outlook. Il flusso e' diverso: qui
non c'e' un JSON da scaricare ne' un redirect su loopback. L'app Azure e' un
client pubblico (nessun client secret) e l'autorizzazione avviene inserendo un
codice su una pagina Microsoft, quindi bastano la libreria standard e nessuna
dipendenza esterna.

Uso:
    python3 msgraph_oauth_setup.py

Nessun argomento e nessun percorso assoluto: lo script lavora sulla cartella in
cui e' posizionato. Il client id viene riusato dall'header se gia' presente,
altrimenti viene chiesto una volta. I valori non vengono stampati.

Requisiti dell'app su https://entra.microsoft.com (App registrations):
  - tipo di account: qualsiasi directory organizzativa + account Microsoft
    personali (coerente con il tenant "common" usato qui e dal firmware)
  - Authentication -> Allow public client flows: Si' (obbligatorio per il
    device code flow)
  - API permissions -> Microsoft Graph -> Delegated -> Calendars.Read
"""

import json
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# --- Configurazione -------------------------------------------------------
# Tutto e' relativo alla cartella dello script, cosi' spostarlo altrove lo fa
# lavorare sul progetto che lo ospita senza modifiche.
SCRIPT_DIR = Path(__file__).resolve().parent

# Header di configurazione da aggiornare, cercato accanto allo script.
ENV_FILENAME = "Env.h"

# Tenant dell'authority. Deve restare uguale a quello che il firmware usa per
# rinnovare il token, altrimenti il refresh token non e' spendibile.
TENANT = "common"

# Scope da autorizzare, nella stessa forma che il firmware manda al refresh.
# offline_access e' cio' che rende emettibile il refresh token.
SCOPE = "https://graph.microsoft.com/Calendars.Read offline_access"

# Macro da scrivere nell'header.
MACRO_CLIENT_ID = "MSGRAPH_CLIENT_ID"
MACRO_REFRESH_TOKEN = "MSGRAPH_REFRESH_TOKEN"

# Valori del template che non vanno considerati un client id gia' configurato.
PLACEHOLDER = {"", "00000000-0000-0000-0000-000000000000", "paste_client_id_here"}
# --------------------------------------------------------------------------

AUTHORITY = f"https://login.microsoftonline.com/{TENANT}"
DEVICECODE_URL = f"{AUTHORITY}/oauth2/v2.0/devicecode"
TOKEN_URL = f"{AUTHORITY}/oauth2/v2.0/token"


def percorso_header():
    """Header di configurazione accanto allo script."""
    percorso = SCRIPT_DIR / ENV_FILENAME
    if not percorso.exists():
        sys.exit(f"{percorso} non esiste: crealo (di solito copiando il "
                 f"template accanto) e rilancia.")
    return percorso


def leggi_macro(testo, macro):
    """Ritorna il valore fra virgolette di un #define, o None se assente."""
    m = re.search(r'^[ \t]*#define[ \t]+' + re.escape(macro) + r'[ \t]+"([^"]*)"',
                  testo, re.MULTILINE)
    return m.group(1) if m else None


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


def post_form(url, campi):
    """POST application/x-www-form-urlencoded -> dict JSON. Gli errori OAuth
    arrivano con status 4xx ma con un corpo JSON utile, quindi vanno letti
    invece di essere propagati come eccezione."""
    dati = urllib.parse.urlencode(campi).encode()
    req = urllib.request.Request(url, data=dati,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        corpo = e.read().decode("utf-8", "replace")
        try:
            return json.loads(corpo)
        except json.JSONDecodeError:
            sys.exit(f"Risposta non JSON da {url} (HTTP {e.code}):\n{corpo}")


def chiedi_client_id(testo):
    """Riusa il client id gia' presente nell'header; lo chiede solo la prima
    volta, quando c'e' ancora il placeholder del template."""
    attuale = leggi_macro(testo, MACRO_CLIENT_ID)
    if attuale is not None and attuale not in PLACEHOLDER:
        print(f"Client id gia' presente in {ENV_FILENAME}, lo riuso.")
        return attuale
    print("Client id dell'app Azure (Application (client) ID della pagina\n"
          "Overview della App registration).")
    valore = input("Client id: ").strip()
    if not valore:
        sys.exit("Nessun client id inserito.")
    return valore


def main():
    percorso = percorso_header()
    testo = percorso.read_text(encoding="utf-8")
    client_id = chiedi_client_id(testo)

    avvio = post_form(DEVICECODE_URL, {"client_id": client_id, "scope": SCOPE})
    if "device_code" not in avvio:
        sys.exit("Avvio del device code flow fallito: "
                 + avvio.get("error_description", json.dumps(avvio)))

    # Il messaggio di Microsoft contiene gia' URL e codice da digitare.
    print("\n" + avvio.get("message", f"Apri {avvio['verification_uri']} e inserisci "
                                      f"il codice {avvio['user_code']}") + "\n")

    intervallo = int(avvio.get("interval", 5))
    scadenza = time.time() + int(avvio.get("expires_in", 900))

    while True:
        if time.time() > scadenza:
            sys.exit("Codice scaduto senza autorizzazione. Rilancia lo script.")
        time.sleep(intervallo)
        esito = post_form(TOKEN_URL, {
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "client_id": client_id,
            "device_code": avvio["device_code"],
        })
        errore = esito.get("error")
        if not errore:
            break
        if errore == "authorization_pending":
            continue
        if errore == "slow_down":
            # Microsoft chiede di rallentare: il polling era troppo fitto.
            intervallo += 5
            continue
        sys.exit("Autorizzazione fallita: "
                 + esito.get("error_description", errore))

    refresh_token = esito.get("refresh_token")
    if not refresh_token:
        sys.exit("Nessun refresh token nella risposta: verifica che l'app abbia\n"
                 "il permesso delegato offline_access oltre a Calendars.Read.")

    aggiorna_header(percorso, {
        MACRO_CLIENT_ID: client_id,
        MACRO_REFRESH_TOKEN: refresh_token,
    })

    print(f"\n{percorso.name} aggiornato: {MACRO_CLIENT_ID}, {MACRO_REFRESH_TOKEN}"
          f"\nScope autorizzati: {SCOPE}")


if __name__ == "__main__":
    main()
