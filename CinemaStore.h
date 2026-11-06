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
 *
 * Lo slot inattivo ospita anche il percorso di routefinder, con un magic suo:
 * il cinema non si scarica mai mentre il percorso è la sorgente del riquadro,
 * quindi scrivere il percorso non tocca l'immagine cinema attiva, che torna a
 * schermo a fine fascia senza download. Un download del cinema lo sovrascrive.
 * Usato dal .ino (fetch del cinema e del percorso, drawTestBackground, cinemaReady).
 * @Mattia Alesi
 */
namespace CinemaStore
{
  /** Contenuto di uno slot: misura e formato sono gli stessi, cambia il magic. */
  enum class Kind : uint8_t { Cinema, Route };

  namespace detail
  {
    /** Intestazione di uno slot: senza magic, o con misure diverse da quelle
     *  del pannello montato, lo slot non contiene un'immagine utilizzabile.
     *  Per il percorso `sequence` porta l'epoch del download, che serve a
     *  decidere se è ancora mostrabile. */
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
    inline constexpr uint32_t ROUTE_MAGIC  = 0x31455452;   // "RTE1" in little endian
    inline constexpr uint32_t IMAGE_OFFSET = sizeof(SlotHeader);
    inline constexpr uint32_t IMAGE_SIZE   = Layout::CINEMA_TOTAL_SZ;

    inline const esp_partition_t*      partition      = nullptr;
    inline uint32_t                    slotSize       = 0;
    inline const uint8_t*              mapped         = nullptr;
    inline esp_partition_mmap_handle_t mapHandle      = 0;
    inline int8_t                      activeSlot     = -1;   // -1: nessuna immagine cinema valida
    inline uint32_t                    activeSequence = 0;
    inline int8_t                      routeSlot      = -1;   // -1: nessun percorso valido
    inline uint32_t                    routeEpoch     = 0;
    inline int8_t                      writingSlot    = -1;   // -1: nessuna scrittura in corso
    inline Kind                        writingKind    = Kind::Cinema;
    inline uint32_t                    writtenBytes   = 0;

    inline GxEPDImage::Descriptor descriptor = {
      GxEPDImage::FORMAT_BWR_1BPP, Layout::CINEMA_W, Layout::CINEMA_H, nullptr, nullptr, nullptr,
    };
    inline GxEPDImage::Descriptor routeDescriptor = {
      GxEPDImage::FORMAT_BWR_1BPP, Layout::CINEMA_W, Layout::CINEMA_H, nullptr, nullptr, nullptr,
    };

    inline uint32_t slotOffset(int8_t slot) { return (uint32_t)slot * slotSize; }

    /** Legge l'intestazione di uno slot; vero se descrive un'immagine con le
     *  misure del pannello montato e con il magic indicato. */
    inline bool readValidHeader(int8_t slot, SlotHeader& header, uint32_t magic)
    {
      if (esp_partition_read(partition, slotOffset(slot), &header, sizeof(header)) != ESP_OK) return false;
      return header.magic == magic && header.width == Layout::CINEMA_W &&
             header.height == Layout::CINEMA_H && header.planes == Layout::CINEMA_PLANES;
    }

    /** Punta un descrittore allo slot indicato, o lo svuota. */
    inline void pointDescriptor(GxEPDImage::Descriptor& d, int8_t slot)
    {
      if (mapped == nullptr || slot < 0)
      {
        d.data0 = nullptr;
        d.data1 = nullptr;
        return;
      }
      const uint8_t* image = mapped + slotOffset(slot) + IMAGE_OFFSET;
      d.data0 = image;
      d.data1 = image + Layout::CINEMA_PLANE_SZ;
    }

    /** Mappa la partizione in memoria dati e punta i descrittori agli slot
     *  attivi; senza mappatura o senza slot valido restano vuoti. */
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
      pointDescriptor(descriptor, activeSlot);
      pointDescriptor(routeDescriptor, routeSlot);
    }

    /** Toglie la mappatura prima di cancellare o scrivere la flash, così la
     *  lettura non dipende dalla coerenza della cache con la scrittura. */
    inline void unmap()
    {
      if (mapped != nullptr) esp_partition_munmap(mapHandle);
      mapped = nullptr;
      pointDescriptor(descriptor, -1);
      pointDescriptor(routeDescriptor, -1);
    }
  }

  /**
   * Trova la partizione, sceglie lo slot cinema valido più recente, ricorda
   * l'eventuale percorso e mappa. Senza partizione, o con una partizione che
   * non contiene due slot, l'archivio resta disattivato e il riquadro mostra
   * il fallback PROGMEM.
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
      if (readValidHeader(slot, header, SLOT_MAGIC) && (activeSlot < 0 || header.sequence > activeSequence))
      {
        activeSlot = slot;
        activeSequence = header.sequence;
      }
      else if (readValidHeader(slot, header, ROUTE_MAGIC))
      {
        routeSlot = slot;
        routeEpoch = header.sequence;
      }
    }
    mapActiveSlot();
    LOG("cinema", "partizione %s, slot da %lu byte, immagine %s, percorso %s",
        partition->label, (unsigned long)slotSize,
        activeSlot < 0 ? "assente" : (activeSlot == 0 ? "nello slot 0" : "nello slot 1"),
        routeSlot < 0 ? "assente" : "presente");
  }

  /** Vero se esiste la partizione dove salvare l'immagine. */
  inline bool isAvailable() { return detail::partition != nullptr; }

  /** Descrittore dell'immagine salvata del tipo indicato, nullptr se non ce
   *  n'è una valida o se è in corso una scrittura. */
  inline const GxEPDImage::Descriptor* image(Kind kind = Kind::Cinema)
  {
    const GxEPDImage::Descriptor& d = kind == Kind::Cinema ? detail::descriptor : detail::routeDescriptor;
    return d.data0 != nullptr ? &d : nullptr;
  }

  /** Epoch del download del percorso salvato, 0 se non ce n'è uno. */
  inline uint32_t routeDownloadedAt() { return detail::routeSlot >= 0 ? detail::routeEpoch : 0; }

  /** Rinuncia alla scrittura in corso e torna a mostrare gli slot attivi. */
  inline void abortWrite()
  {
    detail::writingSlot = -1;
    detail::mapActiveSlot();
  }

  /**
   * Prepara lo slot inattivo del cinema per una nuova immagine cancellandone i
   * settori necessari: va chiamata prima di aprire la connessione, perchè
   * l'erase ferma la cache della flash per circa mezzo secondo. Cinema e
   * percorso scrivono lo stesso slot, quindi il percorso che ci stava è perso
   * da qui in avanti, comunque finisca la scrittura.
   */
  inline bool beginWrite(Kind kind = Kind::Cinema)
  {
    using namespace detail;
    if (partition == nullptr) return false;
    unmap();
    writingSlot = (activeSlot == 0) ? 1 : 0;
    writingKind = kind;
    writtenBytes = 0;
    if (routeSlot == writingSlot) { routeSlot = -1; routeEpoch = 0; }
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
   * rendere valido lo slot. Un cinema diventa l'immagine attiva; un percorso
   * resta accanto al cinema attivo, che non cambia, con `downloadedAt` come
   * istante del download. Un'immagine incompleta viene scartata.
   */
  inline bool commitWrite(uint32_t downloadedAt = 0)
  {
    using namespace detail;
    if (writingSlot < 0 || writtenBytes != IMAGE_SIZE)
    {
      abortWrite();
      return false;
    }
    const bool isRoute = writingKind == Kind::Route;
    const SlotHeader header = {
      isRoute ? ROUTE_MAGIC : SLOT_MAGIC, isRoute ? downloadedAt : activeSequence + 1,
      (uint16_t)Layout::CINEMA_W, (uint16_t)Layout::CINEMA_H, (uint8_t)Layout::CINEMA_PLANES, {0, 0, 0},
    };
    if (esp_partition_write(partition, slotOffset(writingSlot), &header, sizeof(header)) != ESP_OK)
    {
      LOG("cinema", "scrittura dell'intestazione fallita");
      abortWrite();
      return false;
    }
    if (isRoute)
    {
      routeSlot = writingSlot;
      routeEpoch = downloadedAt;
    }
    else
    {
      activeSlot = writingSlot;
      activeSequence = header.sequence;
    }
    writingSlot = -1;
    mapActiveSlot();
    return true;
  }
}

#endif // CINEMA_STORE_H
