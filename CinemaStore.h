#ifndef CINEMA_STORE_H
#define CINEMA_STORE_H

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdint.h>
#include <string.h>

#include "Log.h"

/**
 * Archivio in RAM dell'ultima immagine scaricata per il riquadro in alto a
 * sinistra, percorso o cinema. Sta in RAM e non in flash perchè si riscarica a
 * ogni cadenza e la flash si consumerebbe, e perchè un riavvio la renderebbe
 * comunque non più valida. Un solo buffer basta: le fasce dei due non si
 * sovrappongono e una forzatura ne attiva uno solo.
 *
 * I due piani si allocano separati, perchè due blocchi da metà immagine si
 * trovano contigui più facilmente di uno solo, e non si liberano mai, così
 * l'heap non si frammenta. Se l'allocazione fallisce si ritenta prima di ogni
 * download: il riquadro non si disattiva.
 * Usato dal .ino (download, drawTestBackground) e dalla pagina di stato di
 * Maintenance.h.
 * @Mattia Alesi
 */
namespace CinemaStore
{
  /** Contenuto del buffer. */
  enum class Kind : uint8_t { Cinema, Route };

  namespace detail
  {
    inline uint32_t planeSize      = 0;
    inline uint8_t* planes[2]      = {nullptr, nullptr};
    inline bool     valid          = false;
    inline Kind     kind           = Kind::Cinema;
    inline uint32_t downloadedAt   = 0;   // epoch del download
    inline uint32_t downloadedMs   = 0;   // millis() del download
    inline bool     writing        = false;
    inline Kind     writingKind    = Kind::Cinema;
    inline uint32_t writtenBytes   = 0;

    /** Alloca i piani mancanti nella RAM interna; vero se ci sono entrambi.
     *  Un piano allocato resta, anche se l'altro non trova spazio. */
    inline bool allocate()
    {
      if (planeSize == 0) return false;
      for (uint8_t i = 0; i < 2; ++i)
        if (planes[i] == nullptr)
          planes[i] = (uint8_t*)heap_caps_malloc(planeSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const bool ok = planes[0] != nullptr && planes[1] != nullptr;
      LOG("CinemaStore", "piani da %lu byte %s, heap libero %lu, blocco max %lu",
          (unsigned long)planeSize, ok ? "allocati" : "NON allocati",
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      return ok;
    }
  }

  /**
   * Fissa la misura di un piano e prova ad allocare i due piani. Va chiamata in
   * setup() prima di accendere la radio, quando l'heap ha ancora i blocchi
   * contigui più grandi. Un fallimento qui non è definitivo.
   */
  inline bool begin(uint32_t planeSize)
  {
    detail::planeSize = planeSize;
    return detail::allocate();
  }

  /** Byte di un'immagine completa: i due piani, nero e colore. */
  inline uint32_t imageSize() { return 2U * detail::planeSize; }

  /** Vero se il buffer contiene un'immagine completa. */
  inline bool isValid() { return detail::valid; }

  /** Tipo dell'immagine nel buffer, significativo solo con isValid(). */
  inline Kind kind() { return detail::kind; }

  /** Epoch e millis() del download dell'immagine nel buffer. */
  inline uint32_t downloadedAt() { return detail::downloadedAt; }
  inline uint32_t downloadedMs() { return detail::downloadedMs; }

  /** Piano 0 (nero) o 1 (colore) dell'immagine valida, nullptr se non c'è. */
  inline const uint8_t* plane(uint8_t index)
  {
    return detail::valid && index < 2 ? detail::planes[index] : nullptr;
  }

  /**
   * Prepara il buffer per una nuova immagine. Da qui l'immagine precedente non
   * vale più, quindi il chiamante la chiama solo quando il corpo sta per
   * arrivare. Ritenta l'allocazione se al boot non era riuscita.
   * @return false se i piani non trovano spazio nell'heap.
   */
  inline bool beginWrite(Kind imageKind)
  {
    if ((detail::planes[0] == nullptr || detail::planes[1] == nullptr) && !detail::allocate()) return false;
    detail::valid = false;
    detail::writing = true;
    detail::writingKind = imageKind;
    detail::writtenBytes = 0;
    return true;
  }

  /** Accoda un blocco del corpo, che passa dal piano nero al piano colore
   *  a metà strada. */
  inline bool writeChunk(const uint8_t* data, size_t length)
  {
    uint32_t& written = detail::writtenBytes;
    const uint32_t planeSize = detail::planeSize;
    if (!detail::writing || written + length > imageSize()) return false;
    while (length > 0)
    {
      const uint8_t index = written < planeSize ? 0 : 1;
      const uint32_t offset = written - (uint32_t)index * planeSize;
      const uint32_t room = planeSize - offset;
      const size_t part = length < room ? length : (size_t)room;
      memcpy(detail::planes[index] + offset, data, part);
      data += part;
      length -= part;
      written += (uint32_t)part;
    }
    return true;
  }

  /**
   * Convalida l'immagine scritta, con l'istante del download che ne decide la
   * validità nel riquadro. Un'immagine incompleta si scarta.
   */
  inline bool commitWrite(uint32_t downloadedAtEpoch)
  {
    const bool complete = detail::writing && detail::writtenBytes == imageSize();
    detail::writing = false;
    if (!complete) return false;
    detail::kind = detail::writingKind;
    detail::downloadedAt = downloadedAtEpoch;
    detail::downloadedMs = millis();
    detail::valid = true;
    return true;
  }

  /** Rinuncia alla scrittura in corso: il buffer resta senza immagine, quindi
   *  il riquadro mostra il wallpaper fino al download successivo. */
  inline void abortWrite()
  {
    detail::writing = false;
    detail::valid = false;
  }
}

#endif // CINEMA_STORE_H
