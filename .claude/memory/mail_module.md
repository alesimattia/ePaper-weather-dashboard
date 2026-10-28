---
name: Mail module Gmail
description: Mail.h legge le ultime N mail Gmail e le disegna sotto il wallpaper; token OAuth condiviso con Calendar::Google, una sola connessione TLS in HTTP/1.1 per lista e batch, risposte lette con getString() perchè Gmail risponde chunked
type: project
---

`Mail.h` (header-only, namespace `Mail` + `Mail::detail`) legge le ultime
`MAIL_MAX_MESSAGES` mail Gmail (default `Layout::MAIL_MAX`, una per cella) e le
disegna in una griglia sotto al wallpaper cinema con `Mail::draw()`. API:
`begin()`, `isConfigured()`, `runFetch()`, `draw()`.

**Decisioni di design non ovvie dal codice:**

1. **Token Google condiviso con `Calendar::Google`.**
   `Mail::detail::refreshToken()` è un wrapper su
   `Calendar::detail::refreshGoogleToken()` e `Mail::detail::bearer()`
   ritorna direttamente `Calendar::detail::cachedGoogleToken`. Niente cache
   token locale in Mail. Motivo: durante la finestra OTA il `loop()` gira
   ogni ~10 ms, due cache separate moltiplicherebbero le POST al token
   endpoint. Vincolo: `GOOGLE_REFRESH_TOKEN` in `Env.h` deve essere stato
   emesso con scope unificati `calendar.readonly` + `gmail.readonly`
   (consent unico). Anche le credenziali sono condivise:
   `Mail::isConfigured()` delega a `Calendar::Google::isConfigured()`.

2. **Batch endpoint Gmail per le metadata.** N `messages.get` impacchettati
   in **1 sola POST `multipart/mixed`** verso
   `https://gmail.googleapis.com/batch/gmail/v1`. Il `Bearer` token va solo
   nell'header esterno: il batch endpoint lo propaga alle sub-request.

3. **Una sola connessione TLS per lista e batch.** `runFetch()` crea un client
   (`Net::prepareTlsClient`, handshake limitato a `TLS_HANDSHAKE_TIMEOUT_S`) e un
   `HTTPClient` con `setReuse(true)` e li passa alle due funzioni: un secondo
   `begin()` sullo stesso host riusa la connessione, e `clear()` azzera gli
   header della richiesta precedente (verificato nel core). Se il server chiude
   dopo la lista, HTTPClient si riconnette. Mail resta quindi in **HTTP/1.1**,
   unico modulo: `useHTTP10(true)` disattiverebbe il riuso.

4. **Risposte lette con `getString()`.** Gmail in HTTP/1.1 risponde
   `Transfer-Encoding: chunked` (misurato con curl) e `deserializeJson()` sullo
   stream grezzo fallirebbe; `getString()` decodifica il chunked. Lista e batch
   sono di pochi KB.

5. **`fields=` query filter + `DeserializationOption::Filter` ArduinoJson.**
   Doppia mitigazione (server + client) della pressione di RAM su ESP32.

6. **Wall-clock budget `MAIL_FETCH_BUDGET_MS`** (10 s), controllato fra un
   passo e l'altro: non interrompe un handshake già partito, che ha il suo tetto.
   È un budget di **una esecuzione**, non una cadenza: per questo resta nel
   modulo e non è dello scheduler.

7. **Cadenza, ritenti e backoff non sono di Mail.** Il modulo espone solo
   `runFetch()`, che presuppone la radio già connessa. Il freno all'hammering
   del token endpoint nella finestra di manutenzione è `FETCH_RITENTO_MS` dello
   scheduler (vedi [[fetch_flag_ordering]]). Con credenziali al placeholder il
   task è sospeso dal suo `pronto()` (vedi [[scheduler_task_table]]).

8. **Testo.** Mittente e oggetto passano da `Graphics::foldToFontCharset()`
   subito dopo la copia nei buffer (vedi [[resa_testo_pannello]]).

**How to apply:** se si tocca la struttura del modulo (cambio scope, decoupling
da Calendar), tenere presente che il refresh_token è uno solo e l'access_token
è fisicamente in `Calendar::detail::cachedGoogleToken`. Non duplicare la cache
senza aver risolto prima il problema dell'hammering OTA. Preferenze dell'utente
sulla resa in [[feedback_mail_design]].
