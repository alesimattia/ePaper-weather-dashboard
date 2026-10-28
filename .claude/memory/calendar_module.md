---
name: Modulo calendario (Outlook e Google)
description: Calendar.h, URL Google con timeMin codificato solo nel valore e fields=, items assente come agenda vuota, eventi terminati nascosti con endUtc, data di riferimento dall'orologio, riquadro del mese senza ora valida, guardie sulle credenziali
type: project
---

`Calendar.h` disegna il riquadro del mese e la lista dei prossimi eventi, fusi da
due cache (`outlookEvents[5]`, `googleEvents[5]`), e contiene i due produttori
`Calendar::Outlook` e `Calendar::Google` con la stessa API
(`isConfigured/begin/runFetch`).

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
(vedi [[timing_chain]]).

**Eventi terminati.** `drawEventsList()` salta gli eventi con `endUtc` già
passato, se l'ora è valida. La cache resta quella dell'ultimo fetch riuscito, e
senza questo filtro un fetch in errore li mostrerebbe come prossimi.

**Data di riferimento.** `Weather::detail::renderFrame()` passa a
`Calendar::draw()` l'ora di sistema quando `Clock::valido()`, altrimenti l'epoch
dell'ultimo meteo, altrimenti 0. Con un'ora non valida il riquadro del mese
mostra il titolo "--" senza griglia, e nessun evento risulta "di oggi". Così la
data non dipende dalla riuscita del meteo (vedi [[clock_sntp]]).

**Credenziali.** `isConfigured()` usa `Net::isCredentialSet()` sulle macro di
`Env.h`; i task `google` e `outlook` hanno `pronto` = orologio valido e
credenziali presenti. Con le credenziali Microsoft al placeholder il task outlook
è sospeso e la lista mostra i soli eventi Google. La guardia Google vale anche
per Mail, che condivide le credenziali (vedi [[mail_module]],
[[google_oauth_setup]]).

**Testo.** I titoli passano da `Graphics::foldToFontCharset()` alla ricezione
(vedi [[resa_testo_pannello]]).
