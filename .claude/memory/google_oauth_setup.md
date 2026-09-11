---
name: Autorizzazione Google OAuth (Calendar + Gmail)
description: google_oauth_setup.py scrive le 3 macro GOOGLE_* in Env.h; requisiti del progetto Cloud, i tre errori 403/500 con la causa, e quando il refresh token va rigenerato (l'unico valore non copiabile a mano)
type: project
---

`google_oauth_setup.py` (root del progetto) produce le credenziali Google che
`Env.h` richiede: `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET`,
`GOOGLE_REFRESH_TOKEN`. Si lancia senza argomenti e **non stampa i valori**: apre
il browser, riceve il callback su loopback e riscrive Env.h da sé.

```
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 google_oauth_setup.py
```

**L'interprete è obbligatorio**: il `python3` di default (Homebrew) non ha
tkinter/Pillow/numpy né le dipendenze installate. `google-auth-oauthlib` è
installato nel framework 3.13. Stesso interprete di `epd_image_converter.pyw`.

## Contratto dello script

Tutto è relativo a `SCRIPT_DIR = Path(__file__).resolve().parent`, quindi copiato
altrove agisce sul progetto che lo ospita. Si adatta toccando solo tre costanti in
testa: `ENV_FILENAME`, `SCOPES`, `MACRO`.

- credenziale da due provenienze, stesso percorso di codice (`from_client_config`):
  il `client_secret_*.json` della cartella se c'è, **altrimenti `GOOGLE_CLIENT_ID`
  e `GOOGLE_CLIENT_SECRET` già scritti nell'header** — dopo la prima
  configurazione il JSON è quindi superfluo e il token resta rigenerabile. Con
  due JSON si ferma (sceglierne uno userebbe il progetto sbagliato); senza JSON e
  con l'header ancora a placeholder spiega come scaricarlo;
- dal JSON pretende la sezione `installed`, cioè credenziale di tipo
  **Applicazione desktop** (`web` non ha il redirect su loopback);
- sostituisce solo il testo fra virgolette di `#define` già esistenti, con regex
  `^([ \t]*#define[ \t]+MACRO[ \t]+)"[^"]*"`, quindi **preserva allineamento,
  commenti e ordine** del file; se una macro manca si ferma e la nomina invece di
  appendere righe;
- scrittura **atomica**: tempfile nella stessa dir + `os.replace`, così
  un'interruzione non lascia un Env.h troncato (che bloccherebbe la build);
- `client_secret_*` è in `.gitignore` (Env.h ci era già).

`prompt="consent"` e `access_type="offline"` sono entrambi obbligatori: senza, a un
consenso già dato Google restituisce solo l'access token e il refresh resta vuoto.

## Requisiti del progetto Google Cloud

- progetto **fuori da qualsiasi organizzazione Workspace** (alla creazione:
  "Nessuna organizzazione");
- schermata consenso di tipo **Esterno**;
- **Calendar API e Gmail API abilitate nello stesso progetto**, e una sola app con
  **entrambi** gli scope: `Calendar.h` (`refreshGoogleToken()`) e `Mail.h`
  condividono un unico `GOOGLE_REFRESH_TOKEN` e la stessa cache di access token,
  quindi due app separate non sono utilizzabili — vedi [[mail_module]];
- un refresh token vale solo per gli scope con cui è stato emesso: aggiungerne uno
  dopo richiede di rifare l'autorizzazione da capo.

Progetto in uso: `awesome-gist-508310-j4`, numero **756177182449** (= prefisso del
client id, il modo per verificare di stare guardando il progetto giusto), account
`alesimattia@gmail.com`. Link diretto che evita di editare un progetto omonimo:
`https://console.cloud.google.com/auth/audience?project=<project-id>`.

## I tre errori del consenso, e cosa significano

| Errore | Causa | Rimedio |
|---|---|---|
| `403 org_internal` | schermata consenso **Interna**: il progetto sta dentro un Workspace e accetta solo account di quel dominio | ricreare il progetto senza organizzazione (o passare a Esterno, se l'admin lo consente) |
| `403 access_denied` "app in fase di test" | account non fra gli **utenti di test** — essere proprietario del progetto non include automaticamente | aggiungere l'indirizzo ai tester **e salvare**, oppure pubblicare l'app |
| `500` su accounts.google.com | lato Google, **propagazione**: compare dopo ogni modifica al consenso, anche su `/signin/oauth/v3/consent` subito dopo il passaggio in produzione, con un client id che poco prima funzionava | **riprovare dopo qualche minuto**, si risolve da sé; se insiste, incognito con un solo account loggato |

Il messaggio d'errore nomina il **display name** dell'app, non il progetto: due
progetti con nomi simili si confondono facilmente, ed è per questo che va
verificato il numero progetto contro il prefisso del client id.

## Ciclo di vita: perché lo script serve anche a Env.h già popolato

Le tre macro **non hanno lo stesso ciclo di vita**, ed è la ragione d'essere dello
script:

- `GOOGLE_CLIENT_ID` e `GOOGLE_CLIENT_SECRET` sono proprietà del **progetto
  Cloud**: stabili, copiabili a mano dalla pagina Credenziali;
- `GOOGLE_REFRESH_TOKEN` è il prodotto di un'**autorizzazione interattiva**: non
  esiste nessun posto da cui copiarlo, nasce solo dal consenso nel browser. È
  l'unico valore che lo script produce davvero; gli altri due li riscrive per
  garantire che i tre provengano dalla stessa credenziale e non da due progetti
  mescolati.

Il token va rigenerato — client id e secret già in Env.h, quindi **senza JSON** —
quando:

- viene revocato dalle autorizzazioni dell'account, o cambia la password;
- l'app è rimasta in stato *Test* e sono passati **7 giorni**, o non lo si usa da
  **sei mesi**;
- **cambiano gli scope**: un token non eredita uno scope aggiunto dopo la sua
  emissione, serve riautorizzare da capo;
- si vuole far leggere al pannello un **account Google diverso**: client id e
  secret restano identici, cambia solo chi autorizza.

## Scadenza e stato di pubblicazione

Con l'app in stato **Test** Google invalida i refresh token dopo **7 giorni**: per
un pannello a muro va portata **In produzione** dalla schermata consenso, e poi il
token va **rigenerato**, perché quello emesso in Test si porta dietro la sua
scadenza. Resta un'app non verificata (si passa da *Avanzate → Apri comunque*, con
tetto di 100 utenti): **la verifica di Google non serve** — è per app distribuite a
terzi e con scope Gmail comporta un audit — e non c'entra con la scadenza, che
dipende solo dallo stato *Testing*.

## Gemello per Outlook

`msgraph_oauth_setup.py` fa lo stesso per `MSGRAPH_CLIENT_ID` /
`MSGRAPH_REFRESH_TOKEN` con **device code flow** e **sola stdlib** (nessun MSAL):
l'app Azure è un client pubblico senza secret, quindi non serve né un JSON da
scaricare né un redirect su loopback. `TENANT` e `SCOPE` in testa allo script
ricalcano quelli che `Calendar.h` usa nel refresh — tenant `common`,
`https://graph.microsoft.com/Calendars.Read offline_access` — e vanno tenuti
allineati, altrimenti il refresh token non è spendibile dal firmware. Il client id
viene riusato da Env.h se già scritto, chiesto da stdin solo la prima volta
(quindi va lanciato da un terminale interattivo, o si pre-scrive la macro).
Sull'app Azure serve **Allow public client flows = Sì**, senza cui il device code
flow non parte.
`aggiorna_header()` è duplicata nei due script di proposito: restano autonomi e
copiabili in un altro progetto senza portarsi dietro un modulo comune.
