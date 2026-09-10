---
name: Macro LOG/LOGV e perche' F() e' stato tolto
description: Forma della macro, livello di verbosita' e il motivo non ovvio per cui il format string non va avvolto in F()
type: project
---

`Log.h` definisce `LOG(tag, fmt, ...)` e `LOGV(...)`; nessun `Serial.print*` resta nel firmware fuori dalla macro. Il tag è un **argomento per chiamata**, non una proprietà del file: `Calendar.h` ne usa tre (`[OAuth]`, `[Outlook]`, `[Google]`), e gli header finiscono tutti nella stessa unità di compilazione, quindi un `#define LOG_TAG` per-file "colerebbe" nell'header incluso dopo.

```c
#define LOG_EMIT(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)
```

Il preprocessore concatena tag, testo e a capo in **un unico letterale**: nessuna concatenazione a runtime, `.rodata` identica a scrivere il tag a mano.

**`F()` va tolto, e non è cosmetica.** Su ESP32 `F()` è un semplice cast (`PROGMEM` è vuoto, `PSTR(s)` è `(s)`), quindi non risparmia RAM. Ma se il format string fosse avvolto in `F()` si selezionerebbe l'overload `Print::printf(const __FlashStringHelper*, ...)`, che **non ha `__attribute__((format))**: si perderebbe il controllo del format string su tutti i call site. Con la forma attuale il controllo c'è e la diagnostica punta alla riga della chiamata — ma va compilato con `--warnings more`, perché la build passa `-w`.

**Verbosità a compile-time**, non a runtime: `LOG_LEVEL` 0/1/2 nel `.ino`. La forma spenta è `((void)sizeof(...))`, che in contesto non valutato non genera né codice né stringhe, resta type-checked e marca gli argomenti come usati (niente `-Wunused-variable`). È un'**espressione**, quindi `LOG(...)` funziona come ramo senza graffe di un `if/else` senza il solito `do{}while(0)`.

A `LOG_LEVEL 1` i tre siti per-elemento (`logSlot` di Weather, la riga per messaggio di Mail, il campione BME680) sono `LOGV` e spariscono: sono ~10 righe per ciclo, la metà del totale, ed erano rumore che sommergeva gli esiti.

**Nota sull'output:** i messaggi che prima usavano `println` terminavano con `\r\n` e ora con `\n`. L'output era già misto; adesso è uniforme.
