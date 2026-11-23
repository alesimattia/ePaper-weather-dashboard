#ifndef WALLPAPER_STORE_H
#define WALLPAPER_STORE_H

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

#include "Layout.h"
#include "Log.h"

/**
 * Wallpaper del riquadro in alto a sinistra, nella partizione dati "wallpaper"
 * di partitions.csv e non nel firmware: un aggiornamento OTA non lo ritrasmette
 * e lo si riscrive solo quando cambia, dalla pagina /wallpaper della
 * manutenzione. Il formato è quello di /cinema/arduino, i due piani a misura
 * Layout::CINEMA_W x CINEMA_H, che genera tools/wallpaper_bin.py.
 *
 * Due slot da metà partizione: si scrive sempre quello inattivo e la sua
 * intestazione per ultima, quindi un upload interrotto lascia valido il
 * wallpaper precedente. Lo slot attivo è mappato in memoria e si legge come
 * un'immagine PROGMEM, senza copie in heap.
 * Usato dal .ino (drawTestBackground) e da Maintenance.h (/wallpaper e stato).
 * @Mattia Alesi
 */
namespace WallpaperStore
{
  /** Esito di un caricamento. */
  enum class CommitResult : uint8_t { Saved, Unchanged, Failed };

  namespace detail
  {
    /** Intestazione di uno slot: senza magic, o con misure diverse da quelle
     *  del pannello montato, lo slot non contiene un wallpaper utilizzabile. */
    struct SlotHeader
    {
      uint32_t magic;
      uint32_t sequence;
      uint32_t crc;
      uint16_t width;
      uint16_t height;
      uint8_t  planes;
      uint8_t  reserved[3];
    };
    static_assert(sizeof(SlotHeader) == 20, "WallpaperStore: intestazione di 20 byte");

    inline constexpr uint32_t SLOT_MAGIC   = 0x314C4157;   // "WAL1" in little endian
    inline constexpr uint32_t IMAGE_OFFSET = sizeof(SlotHeader);
    inline constexpr uint32_t IMAGE_SIZE   = Layout::CINEMA_TOTAL_SZ;

    inline const esp_partition_t*      partition      = nullptr;
    inline uint32_t                    slotSize       = 0;
    inline const uint8_t*              mapped         = nullptr;
    inline esp_partition_mmap_handle_t mapHandle      = 0;
    inline int8_t                      activeSlot     = -1;   // -1: nessun wallpaper valido
    inline uint32_t                    activeSequence = 0;
    inline uint32_t                    activeCrc      = 0;
    inline int8_t                      writingSlot    = -1;   // -1: nessuna scrittura in corso
    inline uint32_t                    writtenBytes   = 0;
    inline uint32_t                    writingCrc     = 0;

    inline GxEPDImage::Descriptor descriptor = {
      GxEPDImage::FORMAT_BWR_1BPP, Layout::CINEMA_W, Layout::CINEMA_H, nullptr, nullptr, nullptr,
    };

    inline uint32_t slotOffset(int8_t slot) { return (uint32_t)slot * slotSize; }

    /** Legge l'intestazione di uno slot; vero se descrive un wallpaper con le
     *  misure del pannello montato. */
    inline bool readValidHeader(int8_t slot, SlotHeader& header)
    {
      if (esp_partition_read(partition, slotOffset(slot), &header, sizeof(header)) != ESP_OK) return false;
      return header.magic == SLOT_MAGIC && header.width == Layout::CINEMA_W &&
             header.height == Layout::CINEMA_H && header.planes == Layout::CINEMA_PLANES;
    }

    /** Mappa la partizione e punta il descrittore allo slot attivo; senza
     *  mappatura o senza slot valido resta vuoto. */
    inline void mapActiveSlot()
    {
      if (partition == nullptr) return;
      if (mapped == nullptr &&
          esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA,
                             (const void**)&mapped, &mapHandle) != ESP_OK)
      {
        mapped = nullptr;
        LOG("wallpaper", "mappatura della partizione fallita");
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
   * partizione, o con una partizione troppo piccola per due wallpaper, il
   * riquadro resta senza wallpaper e mostra l'invito a caricarlo.
   */
  inline void begin()
  {
    using namespace detail;
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "wallpaper");
    if (partition == nullptr)
    {
      LOG("wallpaper", "nessuna partizione \"wallpaper\": riflashare con partitions.csv");
      return;
    }
    slotSize = (partition->size / 2) / partition->erase_size * partition->erase_size;
    if (IMAGE_OFFSET + IMAGE_SIZE > slotSize)
    {
      LOG("wallpaper", "partizione da %lu byte troppo piccola per due wallpaper da %lu",
          (unsigned long)partition->size, (unsigned long)IMAGE_SIZE);
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
        activeCrc = header.crc;
      }
    }
    mapActiveSlot();
    LOG("wallpaper", "partizione da %lu byte, wallpaper %s", (unsigned long)partition->size,
        activeSlot < 0 ? "assente: caricarlo da /wallpaper" : "presente");
  }

  /** Descrittore del wallpaper, nullptr se non ce n'è uno valido o se è in
   *  corso un caricamento. */
  inline const GxEPDImage::Descriptor* image()
  {
    return detail::descriptor.data0 != nullptr ? &detail::descriptor : nullptr;
  }

  /** CRC32 del wallpaper attivo, 0 senza wallpaper: lo mostra la pagina di stato. */
  inline uint32_t crc() { return detail::activeSlot >= 0 ? detail::activeCrc : 0; }

  /** Rinuncia al caricamento in corso e torna a mostrare lo slot attivo. */
  inline void abortWrite()
  {
    detail::writingSlot = -1;
    detail::mapActiveSlot();
  }

  /**
   * Prepara lo slot inattivo per un nuovo wallpaper cancellandone i settori
   * necessari: l'erase ferma la cache della flash per circa un secondo, che
   * l'upload in corso assorbe.
   */
  inline bool beginWrite()
  {
    using namespace detail;
    if (partition == nullptr) return false;
    unmap();
    writingSlot = (activeSlot == 0) ? 1 : 0;
    writtenBytes = 0;
    writingCrc = 0;
    const uint32_t erase = (IMAGE_OFFSET + IMAGE_SIZE + partition->erase_size - 1)
                           / partition->erase_size * partition->erase_size;
    if (esp_partition_erase_range(partition, slotOffset(writingSlot), erase) != ESP_OK)
    {
      LOG("wallpaper", "erase dello slot %d fallito", (int)writingSlot);
      abortWrite();
      return false;
    }
    return true;
  }

  /** Accoda un blocco del wallpaper allo slot in scrittura e ne aggiorna la CRC. */
  inline bool writeChunk(const uint8_t* data, size_t length)
  {
    using namespace detail;
    if (writingSlot < 0 || writtenBytes + length > IMAGE_SIZE) return false;
    if (esp_partition_write(partition, slotOffset(writingSlot) + IMAGE_OFFSET + writtenBytes,
                            data, length) != ESP_OK)
      return false;
    writingCrc = esp_rom_crc32_le(writingCrc, data, (uint32_t)length);
    writtenBytes += (uint32_t)length;
    return true;
  }

  /**
   * Chiude il caricamento con l'intestazione, scritta per ultima perchè è lei
   * a rendere valido lo slot. Un wallpaper identico a quello attivo non si
   * convalida: resta quello di prima e l'esito è Unchanged.
   */
  inline CommitResult commitWrite()
  {
    using namespace detail;
    if (writingSlot < 0 || writtenBytes != IMAGE_SIZE)
    {
      abortWrite();
      return CommitResult::Failed;
    }
    if (activeSlot >= 0 && writingCrc == activeCrc)
    {
      abortWrite();
      return CommitResult::Unchanged;
    }
    const SlotHeader header = {
      SLOT_MAGIC, activeSequence + 1, writingCrc,
      (uint16_t)Layout::CINEMA_W, (uint16_t)Layout::CINEMA_H, (uint8_t)Layout::CINEMA_PLANES, {0, 0, 0},
    };
    if (esp_partition_write(partition, slotOffset(writingSlot), &header, sizeof(header)) != ESP_OK)
    {
      LOG("wallpaper", "scrittura dell'intestazione fallita");
      abortWrite();
      return CommitResult::Failed;
    }
    activeSlot = writingSlot;
    activeSequence = header.sequence;
    activeCrc = header.crc;
    writingSlot = -1;
    mapActiveSlot();
    return CommitResult::Saved;
  }
}

#endif // WALLPAPER_STORE_H
