---
name: Firmware OEPL per i tag nRF52811 (Newton) — dual-SSD, pin del cavo pannello, UICR di fabbrica
description: Cosa si ricava dal repo OpenEPaperLink/Tag_FW_nRF52811, che è il firmware della stessa famiglia di tag dei pannelli del progetto - indirizzamento del secondo controller con opcode|0x80 senza secondo CS, segnali BS/VPP/HLT/POWER presenti sul cavo del pannello, tabella UICR con terzo colore e risoluzioni, stato del supporto 12.2"
type: reference
---

Repo <https://github.com/OpenEPaperLink/Tag_FW_nRF52811>. Conta perchè è il firmware della
**famiglia di tag dei pannelli di questo progetto**: la scheda del 12.2" PRO monta un nRF52811
([[gxepd2_122c_driver]]). Sorgenti archiviati in
`A:\epd\GxEPD2_SOLUM_ESL\docs\openepaperlink\nrf52811_tag_fw\` (`dualssd.cpp/.h`, `unissd.cpp`,
`epd_spi.cpp/.h`, `HAL_Newton_M3.h`, `tagtype_db.cpp`); analisi discorsiva in
`docs\fonti_esterne.md`. Da non confondere con `docs\openepaperlink\oepl_display_driver_unissd.c`,
che viene dal firmware "Universal" in C.

## `dualssd.cpp`: due controller SSD su un solo CS, indirizzati con opcode|0x80

```
CONTROLLER_ONE 0x00 / CONTROLLER_TWO 0x80
0x11 → 0x91   0x44 → 0xC4   0x45 → 0xC5   0x4E → 0xCE   0x4F → 0xCF
0x24 → 0xA4   0x26 → 0xA6
```

Un solo CS, un solo BUSY, un solo RST, un solo bus; i comandi comuni (`0x21`, `0x3C`, `0x20`) si
scrivono una volta sola senza offset. **Il `0x21 = 0x08 0x10` dell'init non è un valore magico**:
il secondo byte è `B[4] ckouten`, cioè il bit che mette il master in **cascade** e gli fa emettere
CL verso lo slave (datasheet SSD1683 §6.12, vedi [[ssd1677_command_set]] e [[gxepd2_122c_driver]]).
Nota collegata: anche `unissd.cpp`, che serve pannelli a chip singolo, scrive `0x21` con **due**
byte (`0x08 0x00`) — la forma estesa del SSD1683. I die SOLUM sono di quella generazione, non del
SSD1677 Rev 1.0 che abbiamo come datasheet. Commento nel sorgente: *"Those dual SSD controller
(SSD1683??) behave as 2 400px wide screens"*. Il pannello servito è il 5.85" 792×272, split
sull'asse **X**, ogni metà riceve mezza riga (`buf + XRes/16`), `epdMirrorH` inverte i byte.

**Conseguenza per il 12.2"**: i due COF sono quasi certamente una coppia master/slave con questa
convenzione, quindi niente CS_S e niente GPIO32. Ma indirizzare lo slave **non basta**: in cascade
non ha oscillatore nè booster e dipende dai rail del master, che sul tag arrivano dal fascio di
piste fra i due connettori. Il quadro completo e le evidenze stanno in [[gxepd2_122c_driver]].

## Pin del cavo pannello sul tag di fabbrica (`HAL_Newton_M3.h`, numerazione nRF52811)

`RST 4, BS 2, CS 6, DC 5, BUSY 3, CLK 19, MOSI 20, HLT 23, VPP 24, POWER 7`

- **`EPD_BS` = bus select**, tenuto **LOW** (commento `// low works!`): è la selezione 3 fili / 4
  fili degli SSD16xx. Su una coda cablata a mano un BS flottante può mettere il controller in 3
  fili e farlo ignorare tutto. Sospetto numero uno per la coda muta, e da verificare anche sulla
  coda buona.
- **`EPD_VPP` è usato come MISO** (`NRF_SPI0->PSELMISO = EPD_VPP`, più una lettura a bit-bang):
  **una linea di lettura esiste sul cavo del pannello**. Non contraddice [[waveshare_esp32_driver_board]]
  (sul connettore 24 pin della board il SDO non è instradato), ma sposta il problema: se si trova
  quel contatto sulla coda, `0x2E` / `0x2F` / `0x1B` diventano leggibili e il part number del
  controller si chiude.
- **`EPD_HLT` è il CS di una EEPROM esterna sul pannello** (lo dice il commento): c'è una EEPROM
  sullo stesso bus, sede tipica di waveform/LUT e dati del vetro.
- **`EPD_POWER`** commuta l'alimentazione del pannello: non è sempre alimentato.
- BUSY gestito in due polarità, `EPD_BUSY_SSD` (attivo alto) e `EPD_BUSY_UC` (attivo basso):
  coerente con il BUSY alto dei driver della libreria.

## `tagtype_db.cpp`: la tabella UICR di fabbrica

Byte per tag, con decodifica: `0x09` tipo controller (**0x19 = SSD 9.7**, 0x0A = SSD 4.2/11.6,
0x0F/0x12/0x15 = altre varianti SSD, 0x17 = BWRY), `0x0A` terzo colore, `0x0B-0x0C` Xres,
`0x0D-0x0E` Yres, `0x12-0x13` capability, `0x16` solumType.

**Il byte del terzo colore vale 0x01 = BWR, 0x02 = BWY, 0x03 = BWRY** (0x03 sulle righe 1.6/2.4/3.0
BWRY, 0x02 su "4.2 SSD Yellow"). Le due righe della 9.7" (`9.7 SSD` e `9.7 type 2`) dichiarano
**0x01**, Xres 0x02A0 = 672, Yres 0x03C0 = 960, solumType 0x64 = `STYPE_SIZE_097`, controller 0x19.
Vedi [[gxepd2_097c_driver]]: è l'evidenza più forte sulla questione del quarto colore perchè è dato
di fabbrica scritto nel tag, non catalogo.

`STYPE_SIZE_097` imposta `drawDirectionRight = true`, che scambia le risoluzioni effettive: OEPL usa
lo stesso landscape **960×672** del firmware di questo progetto, senza mirror.

La riga dell'**11.6" BWR** (controller `0x0A`, solumType `0x4A`) dichiara `Xres 0x0280` = **640** e
`Yres 0x03C0` = 960. Serve come terzo punto del conto dei gate: l'SSD1677 ne ha 680, e le taglie
grandi SOLUM sono 672 (9.7"), 640 (11.6") e 768 (12.2") — solo l'ultima sfora, ed è l'unica con due
code FFC. È da qui che la topologia a due controller divisi per righe diventa una derivazione invece
di un'osservazione, vedi [[gxepd2_122c_driver]].

`dualssd` viene istanziato **solo** per 792×272: **OEPL continua a non avere il 12.2"**, e
`oepl-definitions.h` riscaricato è identico alla copia archiviata (nessuna `STYPE_SIZE_122`,
nessuna variante BWRY per la 9.7").

## `unissd.cpp` upstream, ramo 9.7"

`case 0x19` è ora condiviso con `case 0x0A` (11.6") e scrive `0x45 = 00 00 7F 02`, cioè finestra Y
fino a **639** invece di 671, mentre il MUX resta `0x01 = 9F 02` (671). Il resto coincide con la
sequenza di init di fabbrica già documentata in [[gxepd2_097c_driver]], `0x21 = 08 00` compreso.
