#ifndef CINEMA_STORE_H
#define CINEMA_STORE_H

#include <Arduino.h>
#include <esp_partition.h>

#include "Layout.h"
#include "Log.h"

/**
 * Archivio in flash dell'immagine cinema scaricata, così non occupa heap e
 * sopravvive al reboot. Usa due slot della partizione dati "wallpaper" di
 * partitions.csv, oppure della "spiffs" dello schema Minimal SPIFFS se il csv
 * manca: si scrive sempre lo slot inattivo e la sua intestazione per ultima,
 * quindi un download interrotto lascia valida l'immagine precedente.
 * Usato dal .ino (fetchCinemaImage, drawTestBackground, cinemaReady).
 * @Mattia Alesi
 */
namespace CinemaStore
{
  namespace detail
  {
    /** Intestazione di uno slot: senza magic, o con misure diverse da quelle
     *  del pannello montato, lo slot non contiene un'immagine utilizzabile. */
    struct SlotHeader
    {
      uint32_t magic;
      uint32_t sequence;
      uint16_t width;
      uint16_t height;
      uint8_t  planes;
      uint8_t  reserved[3];
    };
    static_assert(sizeof(SlotHeader) == 16, "CinemaStore: intestazione di 16 byte");

    inline constexpr uint32_t SLOT_MAGIC   = 0x31445057;   // "WPD1" in little endian
    inline constexpr uint32_t IMAGE_OFFSET = sizeof(SlotHeader);
    inline constexpr uint32_t IMAGE_SIZE   = Layout::CINEMA_TOTAL_SZ;

    inline const esp_partition_t*      partition      = nullptr;
    inline uint32_t                    slotSize       = 0;
    inline const uint8_t*              mapped         = nullptr;
    inline esp_partition_mmap_handle_t mapHandle      = 0;
    inline int8_t                      activeSlot     = -1;   // -1: nessuna immagine valida
    inline uint32_t                    activeSequence = 0;
    inline int8_t                      writingSlot    = -1;   // -1: nessuna scrittura in corso
    inline uint32_t                    writtenBytes   = 0;

    inline GxEPDImage::Descriptor descriptor = {
      GxEPDImage::FORMAT_BWR_1BPP, Layout::CINEMA_W, Layout::CINEMA_H, nullptr, nullptr, nullptr,
    };

    inline uint32_t slotOffset(int8_t slot) { return (uint32_t)slot * slotSize; }

    /** Legge l'intestazione di uno slot; vero se descrive un'immagine con le
     *  misure del pannello montato. */
    inline bool readValidHeader(int8_t slot, SlotHeader& header)
    {
      if (esp_partition_read(partition, slotOffset(slot), &header, sizeof(header)) != ESP_OK) return false;
      return header.magic == SLOT_MAGIC && header.width == Layout::CINEMA_W &&
             header.height == Layout::CINEMA_H && header.planes == Layout::CINEMA_PLANES;
    }

    /** Mappa la partizione in memoria dati e punta il descrittore allo slot
     *  attivo; senza mappatura o senza slot valido il descrittore resta vuoto. */
    inline void mapActiveSlot()
    {
      if (partition == nullptr) return;
      if (mapped == nullptr &&
          esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA,
                             (const void**)&mapped, &mapHandle) != ESP_OK)
      {
        mapped = nullptr;
        LOG("cinema", "mappatura della partizione fallita");
      }
      if (mapped == nullptr || activeSlot < 0)
      {
        descriptor.data0 = nullptr;
        descriptor.data1 = nullptr;
        return;
      }
      const uint8_t* image = mapped + slotOffset(activeSlot) + IMAGE_OFFSET;
      descriptor.data0 = image;
      descriptor.data1 = image + Layout::CINEMA_PLANE_SZ;
    }

    /** Toglie la mappatura prima di cancellare o scrivere la flash, così la
     *  lettura non dipende dalla coerenza della cache con la scrittura. */
    inline void unmap()
    {
      if (mapped != nullptr) esp_partition_munmap(mapHandle);
      mapped = nullptr;
      descriptor.data0 = nullptr;
      descriptor.data1 = nullptr;
    }
  }

  /**
   * Trova la partizione, sceglie lo slot valido più recente e lo mappa. Senza
   * partizione, o con una partizione che non contiene due slot, l'archivio
   * resta disattivato e il riquadro cinema mostra il fallback PROGMEM.
   */
  inline void begin()
  {
    using namespace detail;
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "wallpaper");
    if (partition == nullptr)
      partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (partition == nullptr)
    {
      LOG("cinema", "nessuna partizione per il wallpaper: riflashare con partitions.csv, resta il fallback");
      return;
    }

    slotSize = (partition->size / 2) / partition->erase_size * partition->erase_size;
    if (IMAGE_OFFSET + IMAGE_SIZE > slotSize)
    {
      LOG("cinema", "partizione %s da %lu byte troppo piccola per due immagini da %lu",
          partition->label, (unsigned long)partition->size, (unsigned long)IMAGE_SIZE);
      partition = nullptr;
      return;
    }

    for (int8_t slot = 0; slot < 2; ++slot)
    {
      SlotHeader header;
      if (readValidHeader(slot, header) && (activeSlot < 0 || header.sequence > activeSequence))
      {
        activeSlot = slot;
        activeSequence = header.sequence;
      }
    }
    mapActiveSlot();
    LOG("cinema", "partizione %s, slot da %lu byte, immagine %s",
        partition->label, (unsigned long)slotSize,
        activeSlot < 0 ? "assente" : (activeSlot == 0 ? "nello slot 0" : "nello slot 1"));
  }

  /** Vero se esiste la partizione dove salvare l'immagine. */
  inline bool isAvailable() { return detail::partition != nullptr; }

  /** Descrittore dell'immagine salvata, nullptr se non ce n'è una valida o se
   *  è in corso una scrittura. */
  inline const GxEPDImage::Descriptor* image()
  {
    return detail::descriptor.data0 != nullptr ? &detail::descriptor : nullptr;
  }

  /** Rinuncia alla scrittura in corso e torna a mostrare lo slot attivo. */
  inline void abortWrite()
  {
    detail::writingSlot = -1;
    detail::mapActiveSlot();
  }

  /**
   * Prepara lo slot inattivo per una nuova immagine cancellandone i settori
   * necessari: va chiamata prima di aprire la connessione, perchè l'erase
   * ferma la cache della flash per circa mezzo secondo.
   */
  inline bool beginWrite()
  {
    using namespace detail;
    if (partition == nullptr) return false;
    unmap();
    writingSlot = (activeSlot == 0) ? 1 : 0;
    writtenBytes = 0;
    const uint32_t erase = (IMAGE_OFFSET + IMAGE_SIZE + partition->erase_size - 1)
                           / partition->erase_size * partition->erase_size;
    if (esp_partition_erase_range(partition, slotOffset(writingSlot), erase) != ESP_OK)
    {
      LOG("cinema", "erase dello slot %d fallito", (int)writingSlot);
      abortWrite();
      return false;
    }
    return true;
  }

  /** Accoda un blocco dell'immagine allo slot in scrittura. */
  inline bool writeChunk(const uint8_t* data, size_t length)
  {
    using namespace detail;
    if (writingSlot < 0 || writtenBytes + length > IMAGE_SIZE) return false;
    if (esp_partition_write(partition, slotOffset(writingSlot) + IMAGE_OFFSET + writtenBytes,
                            data, length) != ESP_OK)
      return false;
    writtenBytes += length;
    return true;
  }

  /**
   * Chiude la scrittura con l'intestazione, scritta per ultima perchè è lei a
   * rendere valido lo slot, e lo rende quello attivo. Un'immagine incompleta
   * viene scartata e resta attivo lo slot precedente.
   */
  inline bool commitWrite()
  {
    using namespace detail;
    if (writingSlot < 0 || writtenBytes != IMAGE_SIZE)
    {
      abortWrite();
      return false;
    }
    const SlotHeader header = {
      SLOT_MAGIC, activeSequence + 1, (uint16_t)Layout::CINEMA_W, (uint16_t)Layout::CINEMA_H,
      (uint8_t)Layout::CINEMA_PLANES, {0, 0, 0},
    };
    if (esp_partition_write(partition, slotOffset(writingSlot), &header, sizeof(header)) != ESP_OK)
    {
      LOG("cinema", "scrittura dell'intestazione fallita");
      abortWrite();
      return false;
    }
    activeSlot = writingSlot;
    activeSequence = header.sequence;
    writingSlot = -1;
    mapActiveSlot();
    return true;
  }
}

#endif // CINEMA_STORE_H
