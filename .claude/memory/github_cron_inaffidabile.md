---
name: I cron di GitHub Actions non servono per un ping a orario preciso
description: Misura su 273 run che ha portato a rimuovere il keep-warm e a spostare il pre-warm nel firmware
type: reference
---

Il repo `cinema-programmation-feed` aveva un workflow che pingava `/health` alle 06:55 locali (due cron UTC per coprire il DST), 5 minuti prima del fetch cinema dell'ESP32 delle 07:00. **Non ha mai funzionato**, e la misura è netta:

| Metrica, su 273 run da aprile a settembre 2026 | Valore |
|---|---|
| Run partiti entro 5 min dal cron | **0 / 273** |
| Run partiti entro 60 min | 38 / 273 (13,9%) |
| Ritardo mediano | **141 min** |
| Ritardo massimo | 1004 min (16 h 44) |
| Ora UTC con più run | 07:00-08:00 (165), contro le 04:55 attese |

Gli eventi `schedule` di GitHub sono best-effort in coda a priorità minima, e un **repo privato su piano free** è la classe più penalizzata. Il punto decisivo non è il ritardo in sé ma che **non è un offset costante da compensare**: va da 9 minuti a 16 ore.

**Vie interne a GitHub, tutte chiuse:** un job che dorme fino all'orario fatturerebbe i minuti di sleep (~3600/mese contro i 2000 gratuiti) e può comunque partire dopo il bersaglio; `workflow_dispatch` parte subito ma richiede un trigger esterno; un repo pubblico darebbe minuti illimitati ma non tocca la latenza, che è il difetto vero.

**Conseguenza:** il pre-warm è stato spostato nel firmware, che è l'unico a sapere quando serve davvero — vedi [[timing_chain]]. Non riaggiungere un cron GitHub per compiti a orario preciso: per un keep-warm servirebbe un pinger esterno (cron-job.org, UptimeRobot), che l'utente ha escluso.
