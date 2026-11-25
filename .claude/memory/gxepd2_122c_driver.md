---
name: Driver custom GxEPD2_SOLUM_122c_960x768 (pannello SOLUM 12.2")
description: Il 12.2" nel submodule - stato del driver (coda lunga = master = righe 384..767 dritta, slave righe 0..383 specchiato X+Y dedotto, init a una passata con cascade 0x91=03 e 0x21=00 10, mai 0x92 nè 0x21=08), controller sconosciuto della famiglia SSD16xx, misure del finder (0x11=03 obbligatorio, TB da G679, 0x46/0x47 inefficaci, bit 7 aliasing sui comandi di sistema), coda corta muta in J1 che esclude il chip indipendente a piedinatura standard, topologia aperta decisa dalla fase 0 di cable_linking.html, esemplare EL122R2CRN (R2, RTL8762ESL), fonti R2/OEPL/atc1441, fonti perse
type: reference
---

Driver del **SOLUM 12.2"**, esemplare `EL122R2CRN` R2 (MCU del tag RTL8762ESL, quarzo 40 MHz, PCB
`HZ-K150HF`, scheda tag originale disponibile), 768w × 960h nativi pilotati 960w × 768h, BWR, code FFC
lunga `FPC-7717` e corta `FPC-7716`. File `GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_122c_960x768.h`; libreria
in [[gxepd2_solum_esl_library]]. Doc: `README_122c.md`, `docs/122c/panel_identification.md`,
`docs/122c/cable_linking.html` (cablaggio della coda corta, fasi 0/1/2a/2, varianti, front-end del
tag), `docs/122c/web_sources/` (fonti web scaricate con provenienza). Strumento:
`examples/12_2c/dual_panel_finder/`. Board in [[waveshare_esp32_driver_board]].

## Controller

Famiglia SSD16xx (comandi `0x12`, `0x24`/`0x26`, `0x22 = F7`, `0x20`), ma il die non coincide con
nessun datasheet noto: TB = 1 funziona e parte da G679, i pattern `0x46`/`0x47` non riempiono la RAM
visualizzata, il secondo byte di `0x21` è accettato con effetto ignoto, LUT in formato da 105 byte.
SSD1677, SSD1683 e SSD2677 valgono solo come analogia: mai scrivere che il die "è" uno di loro.

## Misurato (dual_panel_finder, coda lunga nel J1 della Waveshare)

- La coda lunga dipinge la metà del vetro dal proprio lato, dritta, gate 0 alla giunzione e gate 383
  sul bordo della coda (confermato dall'utente); la coda corta nello stesso connettore non dipinge.
- `0x11 = 0x03` obbligatorio dopo SWRESET: senza, il refresh parte ma il vetro non cambia
  ([inferenza] l'OTP carica i registri al reset e lascia X decrescente).
- MUX: con 384 o 679 si vedono 384 righe, durata uguale entro 13 ms; MUX = `PART_HEIGHT - 1`.
- Polarità: `0x24` bit = 1 bianco. `0x21 = 08` è BW inverse (A[3:0] = 1000): mai nel driver.
- Bit 7 sul master con `0x21` al POR: `0x92`/`0x94`/`0x95` eseguiti come i nudi (stesso BUSY),
  `0xC6`/`0xC7` ignorati. Quindi un comando di sistema con `| 0x80` arriva al master come il nudo:
  `0x92` è un secondo SWRESET del master.
- Tempi: pieno `0xF7` 18,3 s a ambiente; banchi forzati 0 °C 54,8 s, 20 °C 24,3 s, 40/70 °C 17,8 s;
  power on 82 ms, power off 221 ms, SWRESET 2 ms. Push di un piano da 46080 byte: 99 ms a 4 MHz, 43 a
  10, 25 a 20. Partial a LUT custom: 28 frame in 638 ms (20 ms × frame + 83 ms). Mode 2 dell'OTP
  vuoto. Deep sleep `0x10 = 03` perde la RAM.
- Coda corta nel J1 (log grezzi nel commit irraggiungibile `80ee21d` del submodule): BUSY che segue il
  pull, nessuna reazione a SWRESET, `0x47`/`0x46`, `0xC0`, `0x14`, `0x15`, nè in nudo nè a `| 0x80`,
  con reset da 2 a 50 ms. In quella prova c'erano EPD_3.3V, booster e condensatori, BS1 a massa (R11):
  **escluso il controller indipendente con piedinatura standard**. Restano slave in cascade
  (oscillatore spento), piedinatura diversa con CS o enable proprio, chip guasto. La sonda "3 fili"
  del finder non ha valore su questa board (BS1 non si può alzare).

## Stato del driver

| Voce | Valore |
|---|---|
| dispatch | slave righe 0..383 riceve `y`, master righe 384..767 riceve `y - PART_HEIGHT`, in `_writeImage` e `_writeImagePart` |
| specchiature | master (false, false) misurato; slave (true, true) dedotto da un COF gemello ruotato di 180°; ripieghi: banda bassa ribaltata solo in Y -> master (false,true) slave (true,false); solo in X -> master (true,false) slave (false,true); tutta capovolta -> `ROTATION = 2` |
| indirizzamento | runtime: `ADDRESSING_DUAL_CS` (default, `_writeCommandAll` abbassa i due CS insieme, anche per `0x22`/`0x20`) o `ADDRESSING_CASCADE` (slave sul CS del master, solo `0x11/0x44/0x45/0x4E/0x4F/0x24/0x26` a `| 0x80`, BUSY dello slave ignorato) |
| `_InitDisplay` | una sola passata nuda: `0x12`, 200 ms, `0x0C AE C7 C3 C0 80`, `0x01` MUX 383, `0x3C = 01`, `0x18 = 80`, `0x11 = 03`; in cascade in più `0x91 = 03` e `0x21 = 00 10` nudo |
| BUSY slave | `INPUT_PULLDOWN` in `init()`: serve un GPIO con pull (sul firmware GPIO22) |
| refresh | `_Update_Full` `0xF7`; nessuna API partial nel driver; salto del frame invariato via [[frame_skip_unchanged]] |
| tempi | `full_refresh_time` 60000 ms, `busy_timeout` 120 s |

Ordine in cascade: il loop paged scrive prima le pagine dello slave (`| 0x80`) e poi quelle del master
(nudi), come GxEPD2 GDEY0579T93; il firmware R2 del 5,85" fa l'ordine opposto. Entrambi presuppongono
che lo slave ignori le scritture RAM nude: se la banda alta mostra la bassa non è così. `hibernate()` e
`0x22`/`0x20` raggiungono lo slave solo se "nudo = sistema per entrambi" (analogia mai misurata); il
ripiego da provare è `0x90 = 03`.

Nel firmware: `Layout_122c.h` con `SECOND_CONTROLLER_CASCADE` (default false), CS_S 32, BUSY_S 22, SPI
4 MHz, reset 10 ms ([[layout_separation]]).

## Topologia: aperta, la decide la fase 0

- Cascade (H1): il tag ha una sola sezione analogica e un secondo connettore nudo con un fascio di
  piste dal primo (foto FCC delle schede PRO e Core, da verificare sulla scheda R2); un CS, un BUSY.
  La tabella predittiva del README del finder (slave = BUSY fermo su tutto) corrisponde alla misura.
  L'obiezione "la cascade raddoppia le sorgenti" confonde l'uso tipico con un vincolo: due chip in
  cascade che scandiscono ciascuno la propria metà sono un dual-scan.
- CS proprio (H2 con piedinatura diversa o enable): il driver `epd_ti.c` di atc1441 per il 12.2" M3
  in variante "TI" (altro silicio, comandi non SSD16xx, righe da 120 byte divise 60/60 fra i due chip
  cioè split sulle sorgenti) usa CS/CSs e BUSY/BUSYs separati. L'issue OEPL #568 non documenta
  connettori MASTER/SLAVE: è un tag 12.2" guasto.
- Misura decisiva: Ω e diodo sul tag fra connettore nudo, connettore col booster, VSS/VDDIO e pin
  dell'MCU; cattura passiva con analizzatore logico se il tag funziona. Candidati M/S#, CL o enable sui
  pad 1, 4, 6, 7, 19 (J1 non li collega).
- Sequenza R2 di riferimento (5,85" `ELO58R2CRN`, due SSD, atc1441 `epd.c`): EPD_PWR alto, BS basso,
  `0x18 = 80`, `0x22 = B1` + `0x20`, `0x1B = 1A`, `0x10 = 01`, reset hardware, `0x12`, `0x11 = 02` nudo
  e `0x91 = 03`, finestre nude e `C4/C5/CE/CF`, `0x3C = 01`, dati `24/26` e `A4/A6`, `0x20`; nessun
  `0x21`, `0x01`, `0x0C`. OEPL dualssd scrive `0x21 = 08 10`. Pin del tag R2 su 5,85"/9,7": RST P0_0,
  DC P0_1, CS P0_2, CLK P0_4, EPD_PWR P0_5, MOSI P0_6, BS P3_2, BUSY P3_3 (non dimostrati sul 12.2").
- Dump della flash del tag via UART con `rtl8762_loader.py flash-read` in sola lettura, mai write o
  erase prima della copia.

## Fonti perse

Non esistono nel repo nè nella sua storia: `dual_panel_finder.log` con le osservazioni sul vetro,
`docs/122c/pilotaggio_cascade.md`, `docs/122c/my_panel/`, `docs/122c/fonti_web/`,
`docs/122c/firmware_originale/`, lo sketch `rtl8762_uart_bridge`. Le fonti web sono ora in
`docs/122c/web_sources/`.
