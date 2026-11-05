---
name: Autorizzazione Google OAuth (Calendar + Gmail)
description: procedura completa per collegare le API Google al firmware (progetto, DUE API da abilitare, consenso, client desktop, script, verifica, copia di Env.h), progetto in uso, errori del consenso e 403 a runtime con la causa, ciclo di vita del refresh token
type: project
---

## Progetto in uso

id `awesome-gist-508310-j4`, numero **756177182449** (= prefisso di `GOOGLE_CLIENT_ID`: è il modo per
verificare di stare guardando il progetto giusto), account `alesimattia@gmail.com`, nessuna
organizzazione. Nelle URL della console mettere sempre `?project=awesome-gist-508310-j4`: i
progetti con nomi simili si confondono, e il messaggio del consenso nomina il **display name**
dell'app, non il progetto.

## Procedura completa, in ordine

Ogni passo è necessario; il flusso OAuth **non controlla** le API abilitate, quindi un passo
saltato emerge solo a runtime sul dispositivo.

1. **Progetto** fuori da qualsiasi organizzazione Workspace (alla creazione: "Nessuna
   organizzazione"); dentro un'organizzazione il consenso diventa Interno → `403 org_internal`.
2. **Abilitare ENTRAMBE le API** nello stesso progetto, dalla Libreria:
   - Google Calendar API, servizio `calendar-json.googleapis.com`:
     `https://console.cloud.google.com/apis/library/calendar-json.googleapis.com?project=awesome-gist-508310-j4`
   - Gmail API, servizio `gmail.googleapis.com`:
     `https://console.cloud.google.com/apis/library/gmail.googleapis.com?project=awesome-gist-508310-j4`

   Abilitata = la pagina mostra "Gestisci" invece di "Abilita"; elenco completo in
   `/apis/dashboard?project=…`. Propagazione di pochi minuti. Senza la API: il token si rinnova
   (`[Google] token OK`) ma quel modulo prende `403 SERVICE_DISABLED`. Non serve riautorizzare
   né riflashare: il task riprende al primo slot. `gcloud` non è installato sul Mac.
3. **Schermata consenso** (Google Auth Platform, `/auth/overview?project=…`): tipo **Esterno**.
   In `/auth/audience`: aggiungere l'account agli **utenti di test e salvare** (essere proprietari
   non basta), poi **Pubblica app → In produzione**, altrimenti i refresh token scadono dopo 7
   giorni. La verifica Google non serve (vedi sotto).
4. **Client OAuth** in `/auth/clients`: tipo **Applicazione desktop** (il tipo `web` non ha il
   redirect su loopback, lo script lo rifiuta). Scaricare il JSON nella root del repo senza
   rinominarlo: lo copre il `/*.json` del `.gitignore`.
5. **Script** (scrive le tre macro in Env.h, non stampa i valori):
   `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 tools/google_oauth_setup.py`.
   Dal browser: *Avanzate → Apri comunque* (app non verificata), spuntare **entrambi** gli scope
   `calendar.readonly` e `gmail.readonly`.
6. **Verifica dal Mac, prima di flashare**: POST `https://oauth2.googleapis.com/token` con le tre
   macro di Env.h e `grant_type=refresh_token`; il campo `scope` della risposta deve contenere
   entrambi gli scope. Poi GET
   `https://gmail.googleapis.com/gmail/v1/users/me/messages?maxResults=1&fields=messages(id)` col
   bearer: 200 = Gmail ok, altrimenti stampare il corpo d'errore. Lo script di verifica non deve
   stampare i valori delle macro.
7. **Env.h è gitignored**: ogni macchina che compila ne ha una copia propria; dopo una
   rigenerazione va copiata a mano su quella che builda e flasha.

Atteso sul seriale: `[Google] token OK`, `[Mail] fetched N message ids`,
`[Mail] batch parsed N/N messaggi`, `[Google] fetched N events`.

## Errori e causa

| Dove | Errore | Causa | Rimedio |
|---|---|---|---|
| consenso | `403 org_internal` | progetto dentro un Workspace, consenso Interno | progetto nuovo senza organizzazione |
| consenso | `403 access_denied` "app in fase di test" | account non fra gli utenti di test | aggiungerlo **e salvare**, o pubblicare |
| consenso | `500` su accounts.google.com | propagazione lato Google dopo ogni modifica al consenso | riprovare dopo qualche minuto; se insiste, incognito con un solo account |
| seriale | `[OAuth] token refresh failed: http=400` `invalid_grant` | token revocato, password cambiata, app ancora in Test da 7 giorni, sei mesi senza uso | rigenerare il token (passo 5) |
| seriale | `[Mail] list fetch failed: http=403` con `token OK` | corpo `SERVICE_DISABLED` / `accessNotConfigured`: **Gmail API non abilitata** | passo 2 |
| seriale | idem | corpo `ACCESS_TOKEN_SCOPE_INSUFFICIENT`: token emesso senza `gmail.readonly` | passo 5 spuntando entrambi gli scope |

Il firmware logga solo il codice HTTP, mai il corpo: i due 403 di Gmail si distinguono solo con
la verifica del passo 6. Un 403 `SERVICE_DISABLED` è di progetto, quindi vale per qualunque token
e qualunque copia di Env.h.

## Vincoli del firmware

`Calendar.h` (`refreshGoogleToken()`) e `Mail.h` condividono un unico `GOOGLE_REFRESH_TOKEN` e la
stessa cache di access token (vedi [[mail_module]]): una sola app con **entrambi** gli scope, due
app separate non sono utilizzabili. Il refresh omette `scope`, che quindi viene ereditato dal
token. Un token non eredita uno scope aggiunto dopo la sua emissione: aggiungerne uno richiede di
rifare l'autorizzazione da capo.

## Contratto dello script

Tutto è relativo a `PROJECT_DIR`, la cartella superiore a `tools/`, quindi si lancia da qualunque
cwd. Si adatta toccando solo tre costanti in testa: `ENV_FILENAME`, `SCOPES`, `MACRO`.
L'interprete del framework 3.13 è obbligatorio: il `python3` di Homebrew non ha
`google-auth-oauthlib`.

- credenziale da due provenienze, stesso percorso di codice (`from_client_config`): il
  `client_secret_*.json` della root se c'è, **altrimenti `GOOGLE_CLIENT_ID` e
  `GOOGLE_CLIENT_SECRET` già scritti nell'header**: dopo la prima configurazione il JSON è
  superfluo e il token resta rigenerabile. Con due JSON si ferma; senza JSON e con l'header a
  placeholder spiega come scaricarlo;
- dal JSON pretende la sezione `installed`;
- sostituisce solo il testo fra virgolette di `#define` esistenti con regex
  `^([ \t]*#define[ \t]+MACRO[ \t]+)"[^"]*"`, preservando allineamento, commenti e ordine; se una
  macro manca si ferma e la nomina;
- scrittura **atomica** (tempfile nella stessa dir + `os.replace`).

`prompt="consent"` e `access_type="offline"` sono entrambi obbligatori: senza, a un consenso già
dato Google restituisce solo l'access token e il refresh resta vuoto.

## Ciclo di vita delle tre macro

- `GOOGLE_CLIENT_ID` e `GOOGLE_CLIENT_SECRET` sono del **progetto Cloud**: stabili, copiabili a
  mano da `/auth/clients`;
- `GOOGLE_REFRESH_TOKEN` nasce solo dal consenso nel browser, non è copiabile da nessuna parte:
  è l'unico valore che lo script produce davvero; gli altri due li riscrive per garantire che i tre
  vengano dalla stessa credenziale.

Si rigenera il solo token, senza JSON, quando: viene revocato o cambia la password; l'app era in
Test e sono passati 7 giorni, o non lo si usa da sei mesi; cambiano gli scope; si vuole leggere un
**account Google diverso** (client id e secret restano identici).

Stato di pubblicazione: con l'app in **Test** i refresh token scadono dopo 7 giorni; dopo il
passaggio **In produzione** il token va rigenerato, perchè quello emesso in Test si porta dietro la
scadenza. Resta un'app non verificata (tetto di 100 utenti): la verifica Google è per app
distribuite a terzi, con scope Gmail comporta un audit e non c'entra con la scadenza.

## Gemello per Outlook

`tools/msgraph_oauth_setup.py` fa lo stesso per `MSGRAPH_CLIENT_ID` / `MSGRAPH_REFRESH_TOKEN` con
**device code flow** e sola stdlib (nessun MSAL): l'app Azure è un client pubblico senza secret,
quindi niente JSON nè redirect su loopback. `TENANT` e `SCOPE` in testa allo script ricalcano
quelli del refresh in `Calendar.h` (tenant `common`,
`https://graph.microsoft.com/Calendars.Read offline_access`) e vanno tenuti allineati. Il client id
viene riusato da Env.h se già scritto, chiesto da stdin solo la prima volta (terminale
interattivo, o macro pre-scritta). Sull'app Azure serve **Allow public client flows = Sì**.
`aggiorna_header()` è duplicata nei due script di proposito: restano autonomi e copiabili.
