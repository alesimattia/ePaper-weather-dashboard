---
name: Finestra di manutenzione (Maintenance.h)
description: Macchina a stati STA con fallback AP, pagine /config e /update, e le quattro trappole che rompono l'upload
type: project
---

`Maintenance.h` è il modulo della finestra di servizio: l'aggiornamento del firmware è una delle sue pagine, non il suo scopo.

**Stati:** `Inattiva → AttesaSta → ServerSta | ServerAp → Inattiva`. A boot la STA sale in background; se ottiene un indirizzo entro `MAINT_STA_TIMEOUT_S` il server è sulla rete di casa (IP o `epd-dashboard.local` via mDNS), altrimenti si accende l'AP di emergenza. Il fallback usa `WIFI_AP_STA` e **non** `WIFI_AP`: la STA continua a tentare, così un router più lento del dispositivo (tipico dopo un blackout) non condanna il giro alla sola rete di emergenza.

`server.begin()` è chiamato **una volta sola**: `WebServer` ascolta su `0.0.0.0`, quindi serve qualunque interfaccia abbia un indirizzo. La macchina a stati governa solo radio, mDNS e scadenza — nessun rebinding.

**La scadenza parte quando il server diventa raggiungibile**, non a boot, e ogni richiesta la prolunga di 60 s fino a un tetto del doppio della finestra. Senza, una pagina aperta a ridosso della fine porterebbe a un upload rifiutato a metà.

**Chiusura:** stessa sequenza di `wifiOff()` (`disconnect(true, false)`, cache BSSID conservata) e radio consegnata **spenta** allo scheduler. Non riapribile senza riavvio.

## Le quattro trappole, tutte verificate sul core

Ognuna rompe l'upload se ignorata, e nessuna dà un errore chiaro:

1. **Mai chiamare `server.collectHeaders()`.** La chiama `updater.setup()` con `Origin` e `Host` per il controllo CSRF; una seconda chiamata ne *sostituisce* la lista e ogni upload fallisce con "Wrong origin received". I due header restano comunque disponibili ai nostri handler, e infatti li riusiamo per il controllo di origine sulle POST di `/config`.
2. **L'`action` del form deve restare relativa** (`action=/update`), per lo stesso controllo CSRF.
3. **Le credenziali della Basic Auth vanno passate a `updater.setup()`.** L'upload è atomico dentro una sola `handleClient()`: parsing multipart → `Update.end(true)` → `ESP.restart()`. Un controllo a valle arriverebbe dopo che la partizione è già scritta. Per lo stesso motivo la deadline della finestra **non può** interrompere un upload iniziato.
4. **Lo schema di partizioni deve avere due slot applicative** (`No FS 4MB`, 1984 KB ciascuna). Con `Huge APP (3MB No OTA)` c'è una sola slot e `Update.begin()` fallisce.

## Costi misurati

mDNS pesa **~34 KB di flash** (spegnibile con `MAINT_MDNS 0`, resta l'IP). Basic Auth ~344 byte. Con tutto attivo il firmware sta al 67% (9.7") e 69% (12.2") dello slot.

## Sicurezza, detta chiaramente

Sulla rete di casa la pagina è raggiungibile da chiunque sia sulla LAN per la durata della finestra, e `/update` accetta un firmware qualunque — che conterrebbe le credenziali di `Env.h`. Le mitigazioni sono proporzionate a un dispositivo domestico: finestra breve solo a boot con tetto assoluto, Basic Auth opzionale (`MAINT_HTTP_PASSWORD` in `Env.h`, **da definire**), controllo dell'origine sulle POST. Niente HTTPS, niente token.
