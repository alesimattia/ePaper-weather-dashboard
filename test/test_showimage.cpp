/**
 * Verifica di GxEPDImage::showImage() come wrapper di drawInvertedBitmap():
 * con Paint::InkOnly si dipingono solo nero e rosso, con il nero sopra il
 * rosso, e il bianco lascia la pagina com'è; con Paint::Opaque anche il bianco;
 * le righe sopra la pagina corrente si saltano con il page-hint del driver e il
 * piano parte dalla riga giusta.
 *
 * Include l'header VERO (-I..) con un display finto che implementa
 * drawInvertedBitmap() con lo stesso algoritmo di GxEPD2_3C.
 */
#include "GxEPD2_SOLUM_ESL/src/GxEPDImage.h"
#include <cstdio>
#include <cstring>

uint32_t g_msFinto = 0;
SerialeFinta Serial;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-76s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

static const uint16_t SENTINEL = 0x1234;

struct FakeEpd
{
  int16_t hint = 0;
  int16_t showImagePageHint() const { return hint; }
};

/** Display finto di 16 x 8 pixel, con una sola pagina alta quanto si vuole. */
struct FakeDisplay
{
  static const int W = 16;
  static const int H = 8;
  uint16_t px[H][W];
  FakeEpd epd2;
  uint16_t pageH = H;
  uint8_t rot = 0;
  int drawn = 0;

  FakeDisplay() { for (auto& row : px) for (auto& p : row) p = SENTINEL; }
  uint16_t pageHeight() const { return pageH; }
  uint8_t getRotation() const { return rot; }
  int16_t height() const { return H; }

  void drawPixel(int16_t x, int16_t y, uint16_t color)
  {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    px[y][x] = color;
    ++drawn;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
  {
    for (int16_t j = 0; j < h; ++j)
      for (int16_t i = 0; i < w; ++i) drawPixel(x + i, y + j, color);
  }

  /** Stesso ciclo di GxEPD2_3C::drawInvertedBitmap(): disegna i bit a 0. */
  void drawInvertedBitmap(int16_t x, int16_t y, const uint8_t bitmap[], int16_t w, int16_t h, uint16_t color)
  {
    int16_t byteWidth = (w + 7) / 8;
    uint8_t byte = 0;
    for (int16_t j = 0; j < h; j++)
      for (int16_t i = 0; i < w; i++)
      {
        if (i & 7) byte <<= 1;
        else byte = bitmap[j * byteWidth + i / 8];
        if (!(byte & 0x80)) drawPixel(x + i, y + j, color);
      }
  }
};

/** Piani 16 x 8 tutti bianchi (bit a 1), con stride di 2 byte. */
struct Planes
{
  uint8_t black[16];
  uint8_t red[16];
  Planes() { std::memset(black, 0xFF, sizeof(black)); std::memset(red, 0xFF, sizeof(red)); }
  void ink(uint8_t* plane, int x, int y) { plane[y * 2 + x / 8] &= (uint8_t)~(0x80 >> (x & 7)); }
};

int main()
{
  {
    // Nero in (1,0), rosso in (2,0), nero e rosso insieme in (3,0), resto bianco.
    Planes p;
    p.ink(p.black, 1, 0);
    p.ink(p.red, 2, 0);
    p.ink(p.black, 3, 0);
    p.ink(p.red, 3, 0);
    const GxEPDImage::Descriptor d = GXEPD_BWR_IMAGE(p.black, p.red, 16, 8);

    FakeDisplay ink;
    GxEPDImage::showImage(ink, d, 0, 0, GxEPDImage::Paint::InkOnly);
    ok(ink.px[0][1] == GxEPD_BLACK, "InkOnly: il bit nero diventa nero");
    ok(ink.px[0][2] == GxEPD_RED, "InkOnly: il bit rosso diventa rosso");
    ok(ink.px[0][3] == GxEPD_BLACK, "InkOnly: nero e rosso insieme danno nero, come prima");
    ok(ink.px[0][0] == SENTINEL && ink.px[7][15] == SENTINEL, "InkOnly: il bianco lascia la pagina com'è");
    ok(ink.drawn == 4, "InkOnly: drawPixel solo sull'inchiostro (3 pixel, uno due volte)");

    FakeDisplay opaque;
    GxEPDImage::showImage(opaque, d, 0, 0, GxEPDImage::Paint::Opaque);
    ok(opaque.px[0][0] == GxEPD_WHITE && opaque.px[7][15] == GxEPD_WHITE, "Opaque: il bianco si dipinge");
    ok(opaque.px[0][1] == GxEPD_BLACK && opaque.px[0][2] == GxEPD_RED && opaque.px[0][3] == GxEPD_BLACK,
       "Opaque: nero e rosso come in InkOnly");

    FakeDisplay byDefault;
    GxEPDImage::showImage(byDefault, d);
    ok(byDefault.px[0][0] == GxEPD_WHITE, "senza argomento il modo è Opaque");
  }

  {
    // Pagina 1 di altezza 4: le righe 0..3 dell'immagine stanno sopra e si saltano.
    Planes p;
    p.ink(p.black, 0, 1);
    p.ink(p.black, 5, 5);
    p.ink(p.red, 6, 6);
    const GxEPDImage::Descriptor d = GXEPD_BWR_IMAGE(p.black, p.red, 16, 8);
    FakeDisplay page;
    page.pageH = 4;
    page.epd2.hint = 1;
    GxEPDImage::showImage(page, d, 0, 0, GxEPDImage::Paint::InkOnly);
    ok(page.px[1][0] == SENTINEL, "page-hint: la riga sopra la pagina non si disegna");
    ok(page.px[5][5] == GxEPD_BLACK && page.px[6][6] == GxEPD_RED,
       "page-hint: le righe della pagina partono dal byte giusto dei due piani");
    ok(page.drawn == 2, "page-hint: si disegnano solo le righe dalla pagina in giù");
  }

  {
    // Immagine B/N senza piano rosso, spostata di (4, 2).
    Planes p;
    p.ink(p.black, 0, 0);
    const GxEPDImage::Descriptor d = GXEPD_BW_IMAGE(p.black, 16, 8);
    FakeDisplay bw;
    GxEPDImage::showImage(bw, d, 4, 2, GxEPDImage::Paint::InkOnly);
    ok(bw.px[2][4] == GxEPD_BLACK && bw.drawn == 1, "B/N: un solo piano, con l'offset del chiamante");
  }

  std::printf("\n%s\n", falliti == 0 ? "tutti i casi superati" : "CI SONO CASI FALLITI");
  return falliti == 0 ? 0 : 1;
}
