---
name: Metodi nativi prima, niente duplicati della base GxEPD2
description: ordine di preferenza dei metodi (GxEPD2 nativo, poi driver GxEPD2_SOLUM, poi codice epd solo per la GUI) e divieto di duplicare nei driver membri o metodi della base GxEPD2_EPD
type: feedback
---

Ordine di utilizzo dei metodi: prima quelli nativi di GxEPD2 (`GxEPD2_EPD`, `GxEPD2_3C`, Adafruit_GFX); se non esistono, quelli del driver custom in `GxEPD2_SOLUM_ESL`; nel codice di `A:\epd` (`Graphics.h` e moduli) si creano metodi solo quando non riguardano il driver ma la specifica interfaccia grafica della dashboard. Nei driver del submodule nessun membro o metodo duplica la base: un virtuale nativo si sovrascrive con `override` invece di introdurne un omonimo con altro nome, un membro protetto della base si usa invece di ridichiararlo, una funzione della base (init, reset, attesa del BUSY, scrittura di comando e dato) si chiama ed eventualmente si estende.

**Why:** richiesta esplicita dell'utente ("metodi nativi gxepd2 e se non presente usa metodi del custom driver gxepd2-solum e solo se il metodo non riguarda il driver bensì la specifica implementazione della mia interfaccia grafica allora puoi creare metodi nel codice di epd", "non voglio avere metodi duplicati rispetto alla base gxepd2"). Un membro ridichiarato nasconde quello della base, e i metodi della base, `init()` per primo, aggiornano la copia della base e non quella del driver.

**How to apply:** prima di dichiarare un membro o un metodo in un driver cercarlo in `GxEPD2_EPD.h`/`.cpp` e `GxEPD2_3C.h` del clone `A:\tmp\GxEPD2-master`, che coincide con la copia compilata dalla toolchain, e verificare sul codice che il duplicato sia reale. Quando un nome della base ha un altro significato (`_sck`/`_mosi` servono alla lettura software dei registri) si rinomina il membro del driver invece di riusarlo. Vedi [[gxepd2_solum_esl_library]], [[gxepd2_097c_driver]], [[gxepd2_122c_driver]].
