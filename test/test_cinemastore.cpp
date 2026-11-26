/**
 * Verifica di CinemaStore.h, l'archivio in RAM dell'immagine di percorso e
 * cinema: allocazione dei due piani ritentata quando fallisce, corpo diviso
 * fra piano nero e piano colore a blocchi di qualunque misura, immagine
 * convalidata solo se completa e invalidata da un nuovo download.
 *
 * Include l'header VERO del firmware; l'heap è lo stub di
 * stub/esp_heap_caps.h, che si può far fallire a comando.
 */
#include <cstdio>
#include <cstring>

#include "CinemaStore.h"

uint32_t g_msFinto = 0;
SerialeFinta Serial;

static int falliti = 0;
static void ok(bool c, const char* nome)
{
  std::printf("%-64s %s\n", nome, c ? "ok" : "FALLITO");
  if (!c) ++falliti;
}

/** Scrive un'immagine di prova a blocchi di `blocco` byte: il byte i vale i % 251. */
static bool scriviImmagine(uint32_t dimensione, size_t blocco)
{
  static uint8_t buffer[64];
  uint32_t scritti = 0;
  while (scritti < dimensione)
  {
    const size_t n = dimensione - scritti < blocco ? dimensione - scritti : blocco;
    for (size_t i = 0; i < n; ++i) buffer[i] = (uint8_t)((scritti + i) % 251);
    if (!CinemaStore::writeChunk(buffer, n)) return false;
    scritti += (uint32_t)n;
  }
  return true;
}

/** Vero se i due piani contengono la sequenza scritta da scriviImmagine(). */
static bool pianiCorretti(uint32_t piano)
{
  for (uint32_t i = 0; i < 2 * piano; ++i)
  {
    const uint8_t* p = CinemaStore::plane(i < piano ? 0 : 1);
    if (p == nullptr || p[i % piano] != (uint8_t)(i % 251)) return false;
  }
  return true;
}

int main()
{
  const uint32_t PIANO = 1000;   // non multiplo dei blocchi, così un blocco scavalca i piani

  g_heapEsaurito = true;
  ok(!CinemaStore::begin(PIANO), "heap esaurito al boot: piani non allocati");
  ok(!CinemaStore::beginWrite(CinemaStore::Kind::Cinema), "  e il download non parte finchè manca spazio");
  ok(!CinemaStore::isValid() && CinemaStore::plane(0) == nullptr, "  nessuna immagine");

  // ensureAllocated() espone solo l'allocazione: il .ino la chiama prima della
  // GET per scoprire un heap esaurito senza spendere l'handshake.
  ok(!CinemaStore::ensureAllocated(), "ensureAllocated con l'heap esaurito: false");
  ok(!CinemaStore::writeChunk((const uint8_t*)"x", 1), "  e non ha avviato nessuna scrittura");

  g_heapEsaurito = false;
  ok(CinemaStore::ensureAllocated() && !CinemaStore::isValid(),
     "heap tornato: ensureAllocated alloca i piani senza avviare un download");
  ok(!CinemaStore::writeChunk((const uint8_t*)"x", 1), "  nessuna scrittura avviata");
  ok(CinemaStore::beginWrite(CinemaStore::Kind::Cinema), "il download parte con i piani già allocati");
  ok(scriviImmagine(2 * PIANO, 7), "corpo a blocchi da 7 byte, uno a cavallo dei due piani");
  ok(!CinemaStore::isValid(), "  prima del commit l'immagine non vale");
  g_msFinto = 42000;
  ok(CinemaStore::commitWrite(1790000000u), "commit di un'immagine completa");
  ok(CinemaStore::isValid() && CinemaStore::kind() == CinemaStore::Kind::Cinema &&
         CinemaStore::downloadedAt() == 1790000000u && CinemaStore::downloadedMs() == 42000,
     "  valida, di tipo cinema, con epoch e millis del download");
  ok(pianiCorretti(PIANO), "  piano nero e piano colore con i byte giusti");

  // Con un'immagine valida ensureAllocated non la invalida, nemmeno a heap
  // esaurito: i piani ci sono già e non serve allocare niente.
  g_heapEsaurito = true;
  ok(CinemaStore::ensureAllocated() && CinemaStore::isValid() && CinemaStore::downloadedAt() == 1790000000u &&
         pianiCorretti(PIANO),
     "ensureAllocated con un'immagine valida: true, l'immagine resta valida e intatta");
  g_heapEsaurito = false;

  ok(!CinemaStore::writeChunk((const uint8_t*)"x", 1), "nessuna scrittura fuori da un download");

  ok(CinemaStore::beginWrite(CinemaStore::Kind::Route), "nuovo download del percorso");
  ok(!CinemaStore::isValid() && CinemaStore::plane(0) == nullptr, "  il cinema non vale più da subito");
  ok(scriviImmagine(PIANO + 10, 64), "corpo interrotto dopo il primo piano");
  ok(!CinemaStore::commitWrite(1790000300u) && !CinemaStore::isValid(), "  commit di un'immagine incompleta rifiutato");

  ok(CinemaStore::beginWrite(CinemaStore::Kind::Route), "nuovo tentativo del percorso");
  ok(scriviImmagine(2 * PIANO, 64), "  corpo completo a blocchi da 64 byte");
  ok(!CinemaStore::writeChunk((const uint8_t*)"x", 1), "  un byte oltre la misura rifiutato");
  ok(CinemaStore::commitWrite(1790000600u) && CinemaStore::kind() == CinemaStore::Kind::Route &&
         pianiCorretti(PIANO),
     "  commit: percorso valido con i byte giusti");

  ok(CinemaStore::beginWrite(CinemaStore::Kind::Cinema), "download del cinema poi abbandonato");
  CinemaStore::abortWrite();
  ok(!CinemaStore::isValid(), "  abortWrite lascia il buffer senza immagine");

  std::printf("\n%s\n", falliti ? "CI SONO FALLIMENTI" : "tutti i controlli superati");
  return falliti != 0;
}
