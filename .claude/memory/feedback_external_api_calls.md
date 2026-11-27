---
name: feedback-external-api-calls
description: Prima di interrogare un servizio esterno con credenziali dell'utente si dichiara quante chiamate servono e si chiede conferma, partendo dal minimo che dimostra l'ipotesi
metadata:
  type: feedback
---

Prima di chiamare un servizio esterno con le credenziali dell'utente (chiave OpenWeather in `Env.h`, token OAuth di Google e Microsoft Graph, Google Routes e Maps Static di routefinder, cloud Tuya) si dichiarano endpoint e numero di chiamate e si aspetta la conferma. Il piano parte dal minimo che dimostra o smentisce l'ipotesi; altre chiamate si chiedono a parte, solo se il primo esito non basta.

**Why:** la quota è dell'utente e si somma a quella del dispositivo. One Call 3.0 dà 1000 chiamate gratuite al giorno e oltre fattura a consumo, e il dispositivo ne usa già circa 100 con i default. Google Maps Platform fattura a chiamata (vedi [[routefinder_morning_route]]). Chiamate in più per conferme e dettagli consumano quota senza cambiare la diagnosi.

**How to apply:** si scrive il piano prima dello script, per esempio "3 chiamate a `onecall/timemachine` alle 21:00, 21:10 e 21:20". Bastano tre istanti consecutivi a 10 minuti per vedere se `current` alterna fonti diverse. Una chiamata di `timemachine` restituisce un solo istante, quindi il costo è uguale al numero di orari. Vale anche per le sonde di diagnosi e per i test di integrazione lanciati da questa macchina, non solo per l'analisi dei dati.
