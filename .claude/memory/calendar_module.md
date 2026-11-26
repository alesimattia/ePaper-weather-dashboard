---
name: Modulo calendario (Outlook e Google)
description: Calendar.h, Outlook su /me/calendarView da adesso a 30 giorni con isCancelled scartato, URL Google con timeMin codificato solo nel valore e fields=, items assente come agenda vuota, nessun fetch senza ora valida, date validate con evento scartato, titolo vuoto come "(senza titolo)", Bearer e TLS verificato, corpo d'errore del token limitato, eventi terminati nascosti con endUtc, data di riferimento dall'orologio, riquadro del mese senza ora valida, guardie sulle credenziali
type: project
---

`Calendar.h` disegna il riquadro del mese e la lista dei prossimi eventi, fusi da
due cache (`outlookEvents` e `googleEvents`, ciascuna da `MAX_EVENTS_DISPLAYED`
eventi), e contiene i due produttori
`Calendar::Outlook` e `Calendar::Google` con la stessa API
(`isConfigured/begin/runFetch`).

**Outlook.** `GET /me/calendarView` sulla finestra da adesso a `Outlook::HORIZON_DAYS`
(30 giorni), con `$top` pari alla cache, ordine per inizio e `$select` di subject,
start, end, isAllDay e isCancelled; l'orario arriva in UTC con l'header `Prefer`.
calendarView vuole un inizio e una fine e oltre l'orizzonte gli eventi non sono
"prossimi"; espande inoltre le ricorrenze nelle singole occorrenze, mentre
`/me/events` darebbe la serie con la data della prima occorrenza. Un evento con
`isCancelled` resta nella risposta di Graph ma non è un impegno, e si scarta.

**URL di Google.** I `:` vanno codificati in `%3A` **solo nel valore** di
`timeMin`: applicato all'URL intero, `String::replace()` toglie anche quello di
`https:`, e `HTTPClient::begin()` fallisce perchè cerca il protocollo prima del
primo `:`. La richiesta porta `&fields=items(summary,start,end)`: un evento con
descrizione o partecipanti lunghi altrimenti esaurirebbe la memoria del parser a
ogni tentativo. Con `fields=` un calendario vuoto può rispondere senza `items`:
con HTTP 200 e JSON valido vale come agenda vuota, cioè successo. Outlook limita
i campi con `$select`.

**Trasporto.** Token e fetch passano da `Net::beginHttps()` (HTTP/1.0): in
HTTP/1.1 Google e Graph rispondono chunked e il parsing dallo stream fallirebbe
(vedi [[timing_chain]]). Il client è un `Net::TlsClient` con il certificato
verificato contro il bundle CA del core, perchè qui passano token e Bearer; il
Bearer si imposta con `setAuthorizationType("Bearer")` più `setAuthorization()`.
Una risposta 401 ai fetch svuota il token in cache. Il corpo d'errore del token
endpoint si logga per al più 511 byte (buffer da 512 con `Net::readBodyPrefix`),
in una funzione a sè perchè il buffer stia sullo stack solo lì e non durante
l'handshake.

**Ora e date.** Senza ora valida nessuno dei due fetch parte e ritorna false: la
finestra di Outlook partirebbe dal 1970. Le date della risposta si validano in
`ymdhmsToUtcEpoch()` (anno 1970..2100, mese, giorno dentro il mese con gli anni
bisestili, ora, minuti e secondi) e nel parser dell'offset (ore fino a 23, minuti
fino a 59): un evento con una data malformata si scarta e il resto della risposta
si tiene, perchè altrimenti indicizzerebbe fuori dalla tabella dei giorni o
darebbe una data inventata.

**Eventi terminati.** `drawEventsList()` salta gli eventi con `endUtc` già
passato, se l'ora è valida. La cache resta quella dell'ultimo fetch riuscito, e
senza questo filtro un fetch in errore li mostrerebbe come prossimi.

**Data di riferimento.** `Weather::detail::renderFrame()` passa a
`Calendar::draw()` l'ora di sistema quando `Clock::valido()`, altrimenti l'epoch
dell'ultimo meteo, altrimenti 0. Con un'ora non valida il riquadro del mese
mostra il titolo "--" senza griglia, e nessun evento risulta "di oggi". Così la
data non dipende dalla riuscita del meteo (vedi [[clock_sntp]]).

**Credenziali.** `isConfigured()` usa `Net::isCredentialSet()` sulle macro di
`Env.h`; i task `google` e `outlook` hanno `preconditionsMet` = orologio valido e
credenziali presenti. Con le credenziali Microsoft al placeholder il task outlook
è sospeso e la lista mostra i soli eventi Google. Ogni sorgente tiene
`MAX_EVENTS_DISPLAYED` eventi, il massimo fra 7, 6 e 5 che lascia a ogni riga
almeno `MIN_EVT_ROW_H`, perchè una sola sorgente può riempire da sola tutte le
righe (vedi [[layout_invariants]]). Credenziali e scope Google in
[[google_oauth_setup]].

**Testo.** I titoli passano da `setEventTitle()`: `Graphics::foldToFontCharset()` alla
ricezione (vedi [[resa_testo_pannello]]), poi tolti gli spazi in testa e in coda. Un
titolo vuoto, o ridotto a soli spazi dopo la conversione, diventa "(senza titolo)",
altrimenti la riga mostrerebbe solo l'orario.
