---
name: Mail module Gmail
description: Mail.h legge ultime N mail Gmail, riusa OAuth Google del Calendar (token condiviso, batch endpoint), best-effort senza UI
type: project
---

`Mail.h` (header-only, namespace `Mail` + `Mail::detail`) e' stato aggiunto
nel 2026-05 per leggere le ultime `MAIL_MAX_MESSAGES` (default 5) mail
Gmail. **No UI**: solo cache RAM esposta da `Mail::count()` / `Mail::at(i)`,
predisposta per resa grafica futura.

**Why:** l'utente voleva info "a piu' alta priorita'" sotto al meteo
ma ha scelto di non toccare la grafica in questa fase.

**Decisioni di design non ovvie dal codice:**

1. **Token Google condiviso con `Calendar::Google`.**
   `Mail::detail::refreshToken()` e' un wrapper su
   `Calendar::detail::refreshGoogleToken()` e `Mail::detail::bearer()`
   ritorna direttamente `Calendar::detail::cachedGoogleToken`. Niente cache
   token locale in Mail. Motivo: durante la finestra OTA il `loop()` gira
   ogni ~10ms, due cache separate moltiplicherebbero le POST al token
   endpoint. Vincolo: `GOOGLE_REFRESH_TOKEN` in `Env.h` deve essere stato
   emesso con scope unificati `calendar.readonly` + `gmail.readonly`
   (consent unico).

2. **Batch endpoint Gmail per le metadata.** N `messages.get` impacchettati
   in **1 sola POST `multipart/mixed`** verso
   `https://gmail.googleapis.com/batch/gmail/v1` (vedi `fetchMessagesBatch`).
   Riduce gli handshake TLS Mail per ciclo da 6 (1 list + 5 get) a 2
   (1 list + 1 batch). Il `Bearer` token va solo nell'header esterno: il
   batch endpoint lo propaga alle sub-request.

3. **`fields=` query filter + `DeserializationOption::Filter` ArduinoJson.**
   Doppia mitigazione (server + client) della pressione di RAM su ESP32.
   Risposta JSON ridotta del ~70% server-side.

4. **Wall-clock budget `MAIL_FETCH_BUDGET_MS`** (default 10s). Mail
   gira PRIMA dei calendari nel `.ino` (richiesta utente): il budget
   garantisce che un fetch mail patologicamente lento non eroda il
   tempo dei fetch calendario successivi nella stessa finestra WiFi.

5. **Backoff `MAX_CALENDAR_ATTEMPTS=2` anche su fallimento del refresh.**
   Senza questo Mail martellerebbe il token endpoint nel loop OTA a 10ms,
   nonostante il token sia condiviso (Calendar::Google ha il proprio
   counter, indipendente da Mail).

**How to apply:** se in futuro si tocca la struttura del modulo
(rendering UI, cambio scope, decoupling da Calendar), tenere presente
che il refresh_token e' uno solo e l'access_token e' fisicamente in
`Calendar::detail::cachedGoogleToken`. Non duplicare la cache senza
aver risolto prima il problema dell'hammering OTA.
