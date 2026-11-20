/**
 * Verifica del confronto del frame con quello già sul vetro
 * (GxEPD2_SOLUM_FrameCrc.h): CRC concatenato dei due piani, completezza del
 * frame, validità del frame mostrato e regole di invalidazione.
 *
 * Include l'header VERO (-I..): la CRC entra come funzione, quindi qui se ne
 * usa una software con la stessa semantica della ROM dell'ESP32, cioè il CRC32
 * di zlib con il valore parziale in ingresso.
 */
#include "GxEPD2_SOLUM_ESL/src/GxEPD2_SOLUM_FrameCrc.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-76s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

/** CRC32 riflesso, polinomio 0xEDB88320, concatenabile come esp_rom_crc32_le. */
static uint32_t softwareCrc(uint32_t crc, const uint8_t* data, uint32_t length)
{
  crc = ~crc;
  for (uint32_t i = 0; i < length; ++i)
  {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

using Frame = GxEPD2_SOLUM_FrameCrc;

static const int16_t HEIGHT = 672;
static const int16_t PAGE_H = 84;
static const int PAGES = 8;
static const uint16_t ROW_BYTES = 120;
static const size_t PAGE_BYTES = size_t(ROW_BYTES) * PAGE_H;

/** Un frame di prova: PAGES pagine per piano, riempite da un seme. */
struct Content
{
  std::vector<uint8_t> black;
  std::vector<uint8_t> color;
  Content(uint8_t blackSeed, uint8_t colorSeed)
      : black(PAGE_BYTES * PAGES), color(PAGE_BYTES * PAGES)
  {
    for (size_t i = 0; i < black.size(); ++i)
    {
      black[i] = (uint8_t)(blackSeed + i * 7);
      color[i] = (uint8_t)(colorSeed + i * 13);
    }
  }
};

/** Passa al frame tutte le pagine di `c`, saltando eventualmente una pagina o cambiando l'ordine. */
static void feed(Frame& f, const Content& c, int skipPage = -1)
{
  f.beginFrame();
  for (int p = 0; p < PAGES; ++p)
  {
    if (p == skipPage) continue;
    f.addPage(c.black.data() + p * PAGE_BYTES, c.color.data() + p * PAGE_BYTES,
              (int16_t)(p * PAGE_H), PAGE_H, ROW_BYTES);
  }
}

int main()
{
  const Content a(1, 2);

  {
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    ok(!f.unchanged(HEIGHT), "primo frame: niente di mostrato, quindi non invariato");
    f.commit(HEIGHT);
    ok(f.shownValid(), "dopo il commit di un frame completo il frame mostrato è valido");

    feed(f, a);
    ok(f.unchanged(HEIGHT), "stesso contenuto: invariato");
    f.endFrame();
  }

  {
    // Un solo byte diverso, in ogni pagina e in ciascun piano, basta a non essere invariato.
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);
    bool tuttiRilevati = true;
    for (int p = 0; p < PAGES; ++p)
    {
      Content b = a;
      b.black[p * PAGE_BYTES + 5] ^= 0x01;
      feed(f, b);
      if (f.unchanged(HEIGHT)) tuttiRilevati = false;
      f.endFrame();
      Content c = a;
      c.color[p * PAGE_BYTES + PAGE_BYTES - 1] ^= 0x80;
      feed(f, c);
      if (f.unchanged(HEIGHT)) tuttiRilevati = false;
      f.endFrame();
    }
    ok(tuttiRilevati, "un bit diverso in una pagina qualunque, nero o colore: non invariato");
  }

  {
    // Il CRC è concatenato nell'ordine delle pagine: scambiare due pagine con contenuto diverso cambia il frame.
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);
    f.beginFrame();
    for (int p = 0; p < PAGES; ++p)
    {
      const int src = (p == 0) ? 1 : (p == 1 ? 0 : p);
      f.addPage(a.black.data() + src * PAGE_BYTES, a.color.data() + src * PAGE_BYTES,
                (int16_t)(p * PAGE_H), PAGE_H, ROW_BYTES);
    }
    ok(!f.unchanged(HEIGHT), "pagine scambiate di posto: non invariato");
  }

  {
    // Frame non confrontabili: pagine mancanti, fuori ordine, piano mancante, scrittura estranea.
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);

    feed(f, a, 7);
    ok(!f.unchanged(HEIGHT), "ultima pagina mancante: non confrontabile");
    f.endFrame();

    feed(f, a, 0);
    ok(!f.unchanged(HEIGHT), "prima pagina mancante: non confrontabile");
    f.endFrame();

    f.beginFrame();
    f.addPage(a.black.data(), a.color.data(), 0, PAGE_H, ROW_BYTES);
    f.addPage(a.black.data() + 2 * PAGE_BYTES, a.color.data() + 2 * PAGE_BYTES, (int16_t)(2 * PAGE_H), PAGE_H, ROW_BYTES);
    ok(!f.unchanged(HEIGHT), "pagina fuori ordine: non confrontabile");
    f.endFrame();

    f.beginFrame();
    for (int p = 0; p < PAGES; ++p)
      f.addPage(a.black.data() + p * PAGE_BYTES, p == 3 ? nullptr : a.color.data() + p * PAGE_BYTES,
                (int16_t)(p * PAGE_H), PAGE_H, ROW_BYTES);
    ok(!f.unchanged(HEIGHT), "piano colore mancante: non confrontabile");
    f.endFrame();

    feed(f, a);
    f.invalidateRun();
    ok(!f.unchanged(HEIGHT), "scrittura estranea nel frame: non confrontabile");
    f.endFrame();
  }

  {
    // Il frame mostrato si invalida quando il vetro cambia, e solo il commit lo rimette.
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);
    f.invalidateShown();
    feed(f, a);
    ok(!f.unchanged(HEIGHT), "dopo un refresh esterno il frame mostrato non vale più");
    f.commit(HEIGHT);
    feed(f, a);
    ok(f.unchanged(HEIGHT), "il commit di un frame completo rimette il frame mostrato");
    f.endFrame();

    // Commit di un frame incompleto: il frame mostrato resta invalido e non viene copiato.
    feed(f, a, 7);
    f.commit(HEIGHT);
    ok(!f.shownValid(), "commit di un frame incompleto: il frame mostrato è invalido");
  }

  {
    // Chiusa la passata, un refresh senza nuove pagine non rilegge i CRC vecchi.
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);
    ok(!f.unchanged(HEIGHT), "dopo il commit l'accumulo è chiuso: nessun confronto senza pagine nuove");
  }

  {
    // Con il confronto spento non si accumula e non si salta mai.
    Frame f(softwareCrc);
    feed(f, a);
    f.commit(HEIGHT);
    feed(f, a);
    ok(!f.unchanged(HEIGHT), "spento: non invariato, anche con contenuto uguale");
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);
    f.setEnabled(false);
    ok(!f.shownValid(), "spegnerlo butta il frame mostrato");
  }

  {
    // Altezza: un frame che non copre lo schermo non è completo.
    Frame f(softwareCrc);
    f.setEnabled(true);
    feed(f, a);
    f.commit(HEIGHT);
    feed(f, a);
    ok(!f.unchanged((int16_t)(HEIGHT + 1)), "altezza diversa da quella coperta: non completo");
  }

  {
    // La semantica della CRC di prova coincide con quella della ROM: concatenare due blocchi vale il CRC dell'unione.
    const uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    ok(softwareCrc(softwareCrc(0, data, 3), data + 3, 3) == softwareCrc(0, data, 6),
       "la CRC di prova è concatenabile come esp_rom_crc32_le");
  }

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i test superati");
  return falliti != 0;
}
