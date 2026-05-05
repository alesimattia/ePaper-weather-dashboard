---
name: Driver custom GxEPD2_122c per pannello SOLUM 12.2"
description: Driver dual-controller UC8179 per il pannello SOLUM Newton-Core 12.2", scelte di design e stato del bring-up
type: project
---

Branch `Solum_12_2` (non ancora committato) contiene un driver custom in
[`c:\epd\GxEPD2_122c_SOLUM_960x768\`](c:\epd\GxEPD2_122c_SOLUM_960x768) per
il pannello SOLUM Newton-Core 12.2" (960×768 BWR, 2 FFC da 21 pin).

**Why:** estende il progetto al nuovo pannello SOLUM 12.2" recuperato da ESL.
Il datasheet `Newton-Core_Specifications.pdf` è solo marketing — il controller
IC non è dichiarato; assumption operativa: UC8179 dual-controller (coerente
con i 2 FFC e con la dimensione fisica vicina al GDEY1248Z51 12.48").

**How to apply:**

- Driver `GxEPD2_122c_SOLUM_960x768.h` è scheletro strutturale dal `GxEPD2_1248c`
  upstream (UC8179 master/slave) + porting custom features dal driver SOLUM
  9.7" del progetto (`namespace GxEPDImage`, bulk-SPI `writeBytes`,
  `_cleanAccentIfDirty`, page-hint, `setPaged()` override). BWR-only (no yellow).
- Sketch di test in `test_12_2/test_12_2.ino`: cicla BIANCO/NERO/ROSSO ogni 60 s
  via `display.clearScreen(black, color)`. Costruttore single-CS per bring-up
  Step 1 (solo master), `USE_DUAL_CONTROLLER` per Step 2 (master + slave).
- Hardware: Waveshare E-Paper ESP32 Driver Board (FFC interno = master,
  GPIO 13/14/15/25/26/27) + breakout 24-pin esterno per slave (CS_S=GPIO33,
  BUSY_S=GPIO35, segnali condivisi in parallelo).
- Bug fix applicati post-review: busy timeout 30s (era 20s, < 25s refresh),
  `_resetDual` usa `_reset_duration` invece di hard-coded 10ms, commento
  `writeScreenBuffer` riallineato (valori RAW, no inversione).
- TODO[VERIFY] da validare al bring-up dual-controller: split master/slave
  (verticale 480/480 cols), reverse scan slave (0x03), valori cmd 0x61
  resolution, sequenza booster cmd 0x06.

**Stato bring-up al 2026-05-05:**

- ✅ **Step 1** (solo master, FFC #1 al connettore Waveshare): metà sinistra
  del pannello aggiorna correttamente i 3 colori. Init UC8179 risponde,
  busy si rilascia in tempi sensati, refresh ~25 s.
- ⏸ **Step 2** (master + slave): in attesa dell'arrivo del secondo breakout
  24-pin. L'utente confermato che procederà col cablaggio simultaneo di
  entrambi i FFC.
- ❗ **Sospetta architettura master-slave gerarchica** (Causa 1): collegando
  solo il FFC #2 (slave) al connettore Waveshare con lo stesso codice
  single-CS, il display **non aggiorna nulla**. Il sospetto è che le tensioni
  high-voltage (VGH/VGL/VCOM) siano generate dal master e distribuite a
  tutto il pannello via PCB di interconnessione interno; lo slave isolato
  ha SPI ma non può fare elettroforesi. Da confermare con multimetro pin-a-pin
  tra i 2 FFC del pannello (col pannello scollegato): se 5+ pin sono condivisi
  (oltre a GND e VCC) → architettura B confermata.
