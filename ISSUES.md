# Punti aperti del driver SOLUM 9.7"

Interventi rimandati sul driver `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_097c_960x672.h` e sulle API opt-in del
partial, che stanno nell'esempio `GxEPD2_SOLUM_ESL/examples/097c/panel_diagnostic` (`PartialProbeDriver.h`).
I riferimenti sono ai nomi delle funzioni, perchè i numeri di riga cambiano: sono quelli del driver nel
commit "BIG OPTIMIZATIONS".

Ogni punto tocca tempi o sequenze misurati sul vetro, quindi va provato sulla board prima di cambiare il
firmware: lo strumento è `examples/097c/differential_check`, che verifica il partial dentro il loop paged.

## 1. `_PowerOn()` esplicito in `_Update_Part()`

- **Dove**: `_Update_Part()`, chiamata a `_PowerOn()` prima di `0x22 = 0xCC` e `0x20`.
- **Osservazione**: `0xCC` comprende già clock e analog, e il driver GxEPD2 dello stesso silicio
  (`gdem/GxEPD2_1330_GDEM133T91`, `_Update_Part()`) manda solo la sequenza di update e poi segna
  `_power_is_on = true`. Il power on esplicito costa circa 82 ms per ogni partial, cioè a quasi ogni
  frame della giornata. Il modello di durata del partial (20 ms per frame più 83 ms di rampa) è stato
  validato con il power on presente, quindi i 83 ms di rampa potrebbero stare comunque dentro il BUSY di
  `0xCC`.
- **Proposta**: togliere `_PowerOn()` da `_Update_Part()` e impostare `_power_is_on = true` dopo l'attesa
  del BUSY. Con l'occasione rimisurare `_PowerOff()` con `0xC3` contro `0x83` ad analog già acceso: il
  confronto 139 contro 221 ms è stato fatto partendo da analog spento, e la giustificazione del commento
  ("82 ms su un ciclo da 24 s") usa il denominatore del refresh pieno, mentre oggi i partial sono la norma.
- **Verifica**: `differential_check` con e senza la chiamata. Devono restare uguali la durata del BUSY
  di `_Update_Part` (righe `_Update_Part : <µs>` della diagnostica GxEPD2), la resa del nero e del bianco
  lungo una catena e la tenuta del testimone rosso.

## 2. Finestra RAM e border riscritti prima di ogni refresh

- **Dove**: `_setPartialRamArea(0, 0, WIDTH, HEIGHT)` in `_Update_Full()`, in `_Update_Part()` e prima
  del pattern hardware in `_fillPlaneByPattern()`; `0x3C = 0x01` sia in `_InitDisplay()` sia in
  `_Update_Full()`.
- **Osservazione**: ogni scrittura imposta da sè la propria finestra, e il pattern hardware ignora la
  finestra; il border di `_InitDisplay()` viene sovrascritto da quello di `_Update_Full()` nello stesso
  frame, e il partial imposta `0xC0` da sè. Il costo è trascurabile (circa 0,3 ms a refresh): il punto è
  la coerenza della sequenza.
- **Proposta**: togliere le finestre ridondanti e lo `0x3C` dell'init, oppure lasciare un commento che
  dica perchè restano, se la misura sul vetro mostra che servono.
- **Verifica**: un refresh pieno e una catena di partial con `differential_check`, controllando bordo,
  resa e durate.

## 3. Limiti delle API opt-in del partial (`panel_diagnostic`)

Le API `drawImagePartial()`, `drawImagePartialPart()`, `refreshPartial()`, `writeImageToPrevious()`,
`writeScreenBufferPrevious()`, `setPartialLut()`, `writeImageBlack()` e `writeImageRed()` servono solo alle
misure dell'esempio e non al firmware, che usa il refresh differenziale dentro il loop paged. Restano
questi difetti, da correggere se un giorno tornassero in una API pubblica:

- **Precondizione senza esito**: `drawImagePartial*()` e `refreshPartial()` non verificano che `0x26`
  contenga il frame precedente. Dopo un frame del template il primo partial passa la guardia di
  `_ensurePartialRamDefined()`, che agisce solo con `_initial_write`, non disegna il nero e fa lampeggiare
  lo schermo. Proposta: farle ritornare `bool` e rifiutare la passata se lo stato di `0x26` non è
  `Previous`, senza nessuna scrittura su `Serial`.
- **`writeImage(bitmap)` dentro una catena**: la scrittura a canale singolo ripulisce `0x26` quando non è
  `Clean`, quindi cancella anche il frame precedente e la catena diventa un refresh pieno. Proposta:
  ripulire solo un accent, lasciando un `Previous` alla pulizia che `_Update_Full()` e `setPaged()` già
  fanno prima che `0x26` venga riletta come accent.
- **Metodi `*Again` della base**: `writeScreenBufferAgain()`, `writeImageAgain()` e `writeImagePartAgain()`
  sono ereditati da `GxEPD2_EPD` con la semantica a canale singolo, non con "corrente uguale al
  precedente" degli altri driver GxEPD2. Proposta: sovrascriverli scrivendo `0x24` e `0x26` con lo stesso
  contenuto e segnando `Previous`; `drawImagePartial()` diventerebbe la forma di `GxEPD2_1160_T91::drawImage`.
- **VGH e VCOM dopo un init**: un partial opt-in subito dopo il boot o un risveglio gira dopo un SWRESET,
  con VGH e VCOM ai valori di reset, perchè solo `0xF7` li ricarica dall'OTP. Il differenziale lo evita
  perchè ogni suo frame pieno riparte da init e chiude con `0xF7`. Proposta: un flag che dica se l'OTP è
  caricato e, se non lo è, un load (`0x22 = 0xB1`, circa 3 ms) prima della LUT del partial.
- **`init(..., initial = false)`**: GxEPD2 lo prescrive al risveglio dell'MCU con l'alimentazione del
  pannello mantenuta, perchè il suo `hibernate()` (`0x10 = 0x01`) conserva la RAM. Qui il deep sleep è
  `0x03`, che la perde, quindi una scrittura a canale singolo seguita da un refresh rileggerebbe come
  accent una RAM indefinita. Proposta: documentare che con questo driver `initial` va sempre `true`, oppure
  forzare `_initial_write` in un override di `init()`.
