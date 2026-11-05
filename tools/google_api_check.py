"""
Verifica da questa macchina che le credenziali Google dell'header di
configurazione funzionino come le usa il firmware, prima di flashare.

Rifà le stesse chiamate dei moduli Calendar e Mail:
  1. rinnovo dell'access token con le tre macro GOOGLE_* (grant_type=refresh_token
     senza scope, come Calendar::detail::refreshGoogleToken());
  2. lettura di un evento del calendario primario (Calendar API);
  3. lettura di un id di messaggio della posta in arrivo (Gmail API).

Stampa gli scope del token, i codici HTTP e, per gli errori, il messaggio e i
motivi di Google, che distinguono le cause di un 403: SERVICE_DISABLED = API non
abilitata nel progetto, ACCESS_TOKEN_SCOPE_INSUFFICIENT = token emesso senza
quello scope. Non stampa i valori delle macro nè il contenuto di eventi e
messaggi.

Uso:
    python3 tools/google_api_check.py

Nessun argomento: lavora sull'header nella root del progetto, la cartella
superiore a tools/. Solo libreria standard. Esce con codice 1 se un passo
fallisce.
"""

import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# --- Configurazione -------------------------------------------------------
# Tutto è relativo alla root del progetto, la cartella superiore a tools/.
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent

# Header di configurazione da leggere, cercato nella root del progetto.
ENV_FILENAME = "Env.h"

# Macro della credenziale, nell'ordine in cui le usa il rinnovo del token.
MACRO = ("GOOGLE_CLIENT_ID", "GOOGLE_CLIENT_SECRET", "GOOGLE_REFRESH_TOKEN")

# Frammenti che indicano un valore ancora al placeholder di Env_template.h.
PLACEHOLDER_FRAMMENTI = ("paste_", "0000000")

TOKEN_URL = "https://oauth2.googleapis.com/token"

# API da verificare -> richiesta minima, con fields= per non scaricare dati.
VERIFICHE = {
    "Calendar API": "https://www.googleapis.com/calendar/v3/calendars/primary/events"
                    "?maxResults=1&fields=items(id)",
    "Gmail API": "https://gmail.googleapis.com/gmail/v1/users/me/messages"
                 "?maxResults=1&labelIds=INBOX&fields=messages(id)",
}
# ---------------------------------------------------------------------------


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


def richiedi(req):
    """Esegue la richiesta e ritorna (codice HTTP, corpo decodificato), anche
    per le risposte d'errore."""
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def descrivi_errore(risposta):
    """Riduce il corpo d'errore di una API Google al messaggio e ai motivi,
    che sono ciò che distingue le cause; il corpo grezzo se non è nel formato
    atteso."""
    try:
        err = json.loads(risposta)["error"]
        voci = err.get("details", []) + err.get("errors", [])
        motivi = dict.fromkeys(v["reason"] for v in voci if "reason" in v)
        return f"{err['message']}\n  motivi: {', '.join(motivi)}"
    except (ValueError, KeyError, TypeError, AttributeError):
        return risposta


def main():
    testo = (PROJECT_DIR / ENV_FILENAME).read_text(encoding="utf-8")
    valori = {m: leggi_macro(testo, m) for m in MACRO}
    mancanti = [m for m, v in valori.items() if v is None]
    if mancanti:
        sys.exit(f"{ENV_FILENAME}: macro assenti o al placeholder: {', '.join(mancanti)}\n"
                 "Lancia prima tools/google_oauth_setup.py.")

    corpo = urllib.parse.urlencode({
        "client_id": valori["GOOGLE_CLIENT_ID"],
        "client_secret": valori["GOOGLE_CLIENT_SECRET"],
        "refresh_token": valori["GOOGLE_REFRESH_TOKEN"],
        "grant_type": "refresh_token",
    }).encode()
    codice, risposta = richiedi(urllib.request.Request(TOKEN_URL, corpo))
    if codice != 200:
        # Il corpo del token endpoint è breve e non contiene segreti
        # (es. {"error": "invalid_grant", ...}).
        sys.exit(f"Rinnovo del token: HTTP {codice}\n{risposta}")
    token = json.loads(risposta)
    print(f"Rinnovo del token: HTTP 200, scope = {token.get('scope')}")

    esito = 0
    for nome, url in VERIFICHE.items():
        req = urllib.request.Request(
            url, headers={"Authorization": "Bearer " + token["access_token"]})
        codice, risposta = richiedi(req)
        if codice == 200:
            print(f"{nome}: HTTP 200")
        else:
            print(f"{nome}: HTTP {codice}\n  {descrivi_errore(risposta)}")
            esito = 1
    sys.exit(esito)


if __name__ == "__main__":
    main()
