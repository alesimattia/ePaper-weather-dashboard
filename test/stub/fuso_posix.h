#pragma once
/**
 * Regole di fuso orario in formato POSIX, per i test su host.
 *
 * Non è un fuso finto: è lo stesso lavoro che fa newlib sull'ESP32, dove non
 * esiste nessun database IANA e l'unica sorgente di verità è la stringa
 * CAL_POSIX_TZ ("CET-1CEST,M3.5.0,M10.5.0/3") passata a setenv()/configTzTime().
 * Qui la stringa viene interpretata da noi invece che dalla libc dell'host, per
 * due ragioni:
 *
 *   - il CRT Windows NON legge i campi di transizione (accetta solo la forma
 *     storica tzn[+|-]hh[dzn] e applica poi le regole statunitensi), quindi
 *     test_scheduler non era compilabile in modo attendibile su Windows;
 *   - il risultato non dipende più dal tz database dell'host nè dalle
 *     impostazioni regionali della macchina, che è ciò che serve a un test.
 *
 * Copre il sottoinsieme che il progetto usa e nient'altro:
 *
 *     STD offset [DST [offset]] , Mm.w.d[/h] , Mm.w.d[/h]
 *
 * con offset [+|-]hh[:mm[:ss]], offset estivo implicito a standard + 1 h e ora
 * di transizione implicita alle 02:00. Le forme Jn e n dei giorni giuliani NON
 * sono supportate e fanno ritornare falso a fusoImposta(): se un domani
 * CAL_POSIX_TZ cambiasse forma deve rompersi il test, non silenziosamente il
 * fuso.
 *
 * Uso: fusoImposta() una volta, poi fusoLocaltime()/fusoMktime() al posto di
 * localtime_r()/mktime(). Il test li aggancia con due #define prima di
 * includere Scheduler.h, come già fa con time().
 */

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace fuso
{
  /** Una regola Mm.w.d/h: mese 1..12, settimana 1..5 (5 = l'ultima), giorno
   *  0..6 con 0 = domenica, ora della transizione in secondi da mezzanotte. */
  struct Regola
  {
    int     mese      = 0;
    int     settimana = 0;
    int     giorno    = 0;
    int32_t ora       = 2 * 3600;
  };

  namespace detail
  {
    inline int32_t offStd = 0;      // secondi a EST di UTC (CET = +3600)
    inline int32_t offDst = 0;
    inline bool    haDst  = false;
    inline Regola  inizio;
    inline Regola  fine;

    /**
     * Giorni dall'epoch per una data civile (algoritmo days-from-civil di
     * Howard Hinnant). È lineare in `giorno`, quindi assorbe da sè un valore
     * oltre la fine del mese: serve a fusoMktime(), perchè sia
     * Scheduler::msFinoAllOra() sia il test fanno tm_mday += 1.
     */
    inline int64_t giorniDaCivile(int anno, int mese, int giorno)
    {
      anno -= mese <= 2;
      const int64_t era = (anno >= 0 ? anno : anno - 399) / 400;
      const int64_t aoe = anno - era * 400;                       // [0, 399]
      const int64_t goa = (153 * (mese + (mese > 2 ? -3 : 9)) + 2) / 5 + giorno - 1;
      const int64_t goe = aoe * 365 + aoe / 4 - aoe / 100 + goa;
      return era * 146097 + goe - 719468;
    }

    /** Inversa della precedente. */
    inline void civileDaGiorni(int64_t z, int& anno, int& mese, int& giorno)
    {
      z += 719468;
      const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const int64_t goe = z - era * 146097;                       // [0, 146096]
      const int64_t aoe = (goe - goe / 1460 + goe / 36524 - goe / 146096) / 365;
      const int64_t a   = aoe + era * 400;
      const int64_t goa = goe - (365 * aoe + aoe / 4 - aoe / 100);
      const int64_t mp  = (5 * goa + 2) / 153;                    // [0, 11]
      giorno = (int)(goa - (153 * mp + 2) / 5 + 1);               // [1, 31]
      mese   = (int)(mp + (mp < 10 ? 3 : -9));                    // [1, 12]
      anno   = (int)(a + (mese <= 2));
    }

    /**
     * Istante UTC della transizione descritta da `r` nell'anno indicato.
     *
     * L'ora della regola è espressa nell'ora LOCALE in vigore PRIMA della
     * transizione, quindi l'offset da sottrarre è quello del chiamante: per
     * l'Europa si ottengono le 01:00 UTC da entrambe le regole, ed è la
     * controprova immediata che il conto è giusto.
     */
    inline int64_t istanteRegola(const Regola& r, int anno, int32_t offInVigore)
    {
      const int64_t primo = giorniDaCivile(anno, r.mese, 1);
      // 1970-01-01 era un giovedì, cioè wday 4.
      const int wdPrimo = (int)(((primo + 4) % 7 + 7) % 7);
      const int delta   = (r.giorno - wdPrimo + 7) % 7;
      int64_t giorno    = primo + delta + (int64_t)(r.settimana - 1) * 7;

      // settimana 5 = "l'ultima": se sfora il mese si torna indietro.
      const int annoSucc = (r.mese == 12) ? anno + 1 : anno;
      const int meseSucc = (r.mese == 12) ? 1 : r.mese + 1;
      const int64_t primoDelSuccessivo = giorniDaCivile(annoSucc, meseSucc, 1);
      while (giorno >= primoDelSuccessivo) giorno -= 7;

      return giorno * 86400 + r.ora - offInVigore;
    }

    /** Anno civile di un istante espresso in secondi dall'epoch. */
    inline int annoDi(int64_t secondi)
    {
      int64_t giorni = secondi / 86400;
      if (secondi % 86400 < 0) --giorni;
      int a, m, g;
      civileDaGiorni(giorni, a, m, g);
      return a;
    }

    /** Vero se l'istante UTC cade nel periodo di ora legale. */
    inline bool inDst(int64_t utc)
    {
      if (!haDst) return false;
      const int anno = annoDi(utc + offStd);
      const int64_t a = istanteRegola(inizio, anno, offStd);
      const int64_t b = istanteRegola(fine, anno, offDst);
      if (a <= b) return utc >= a && utc < b;
      return utc >= a || utc < b;   // emisfero sud: il periodo scavalca l'anno
    }

    /** Salta il nome del fuso: lettere, oppure la forma fra angolari. */
    inline const char* saltaNome(const char* s)
    {
      if (*s == '<')
      {
        while (*s && *s != '>') ++s;
        return *s ? s + 1 : s;
      }
      while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) ++s;
      return s;
    }

    /**
     * Legge [+|-]hh[:mm[:ss]] e lo rende in secondi a EST di UTC.
     * POSIX definisce l'offset come il valore da SOMMARE all'ora locale per
     * ottenere UTC, quindi il segno va ribaltato: "CET-1" significa un'ora a
     * est, cioè locale = UTC + 1 h.
     */
    inline bool leggiOffset(const char*& s, int32_t& out)
    {
      int segno = 1;
      if (*s == '+') ++s;
      else if (*s == '-') { segno = -1; ++s; }
      if (*s < '0' || *s > '9') return false;

      int32_t v[3] = {0, 0, 0};
      for (int i = 0; i < 3; ++i)
      {
        while (*s >= '0' && *s <= '9') v[i] = v[i] * 10 + (*s++ - '0');
        if (i == 2 || *s != ':') break;
        ++s;
        if (*s < '0' || *s > '9') return false;
      }
      out = -segno * (v[0] * 3600 + v[1] * 60 + v[2]);
      return true;
    }

    /** Legge Mm.w.d[/h] dopo la virgola. */
    inline bool leggiRegola(const char*& s, Regola& r)
    {
      if (*s != 'M') return false;   // le forme Jn e n non sono supportate
      ++s;
      int campi[3] = {0, 0, 0};
      for (int i = 0; i < 3; ++i)
      {
        if (*s < '0' || *s > '9') return false;
        campi[i] = 0;
        while (*s >= '0' && *s <= '9') campi[i] = campi[i] * 10 + (*s++ - '0');
        if (i < 2)
        {
          if (*s != '.') return false;
          ++s;
        }
      }
      r.mese = campi[0];
      r.settimana = campi[1];
      r.giorno = campi[2];
      if (r.mese < 1 || r.mese > 12 || r.settimana < 1 || r.settimana > 5 ||
          r.giorno < 0 || r.giorno > 6)
        return false;

      r.ora = 2 * 3600;
      if (*s == '/')
      {
        ++s;
        // Stesso formato dell'offset ma senza inversione di segno.
        int32_t o = 0;
        const char* p = s;
        if (!leggiOffset(p, o)) return false;
        s = p;
        r.ora = -o;
      }
      return true;
    }
  }

  /**
   * Interpreta una stringa POSIX TZ e la rende attiva per le chiamate
   * successive. Va invocata una volta a inizio test.
   * @return false se la stringa è fuori dal sottoinsieme supportato: il
   *         chiamante deve trattarlo come un fallimento, non ignorarlo.
   */
  inline bool fusoImposta(const char* tz)
  {
    using namespace detail;
    offStd = offDst = 0;
    haDst = false;
    inizio = Regola{};
    fine = Regola{};
    if (tz == nullptr || *tz == 0) return false;

    const char* s = saltaNome(tz);
    if (!leggiOffset(s, offStd)) return false;

    if (*s == 0) return true;            // fuso senza ora legale

    s = saltaNome(s);
    if (*s != ',')
    {
      if (!leggiOffset(s, offDst)) return false;
    }
    else
      offDst = offStd + 3600;            // default POSIX: standard + 1 h

    if (*s != ',') return false;
    ++s;
    if (!leggiRegola(s, inizio)) return false;
    if (*s != ',') return false;
    ++s;
    if (!leggiRegola(s, fine)) return false;
    if (*s != 0) return false;

    haDst = true;
    return true;
  }

  /** Come localtime_r(), con il fuso impostato da fusoImposta(). */
  inline struct tm* fusoLocaltime(const time_t* t, struct tm* out)
  {
    using namespace detail;
    const int64_t utc = (int64_t)*t;
    const bool dst = inDst(utc);
    const int64_t locale = utc + (dst ? offDst : offStd);

    int64_t giorni = locale / 86400;
    int32_t resto = (int32_t)(locale % 86400);
    if (resto < 0) { resto += 86400; --giorni; }

    int anno, mese, giorno;
    civileDaGiorni(giorni, anno, mese, giorno);

    std::memset(out, 0, sizeof(*out));
    out->tm_sec  = resto % 60;
    out->tm_min  = (resto / 60) % 60;
    out->tm_hour = resto / 3600;
    out->tm_mday = giorno;
    out->tm_mon  = mese - 1;
    out->tm_year = anno - 1900;
    out->tm_wday = (int)(((giorni + 4) % 7 + 7) % 7);
    out->tm_yday = (int)(giorni - giorniDaCivile(anno, 1, 1));
    out->tm_isdst = dst ? 1 : 0;
    return out;
  }

  /**
   * Come mktime(): normalizza i campi fuori intervallo, risolve l'offset e
   * riscrive il `tm` normalizzato.
   *
   * Con tm_isdst = -1 si prova prima l'offset standard e, se l'istante
   * ottenuto cade in ora legale, si ritenta con quello estivo. Le due ore
   * patologiche del cambio si risolvono così, e il comportamento va conosciuto
   * perchè è raggiungibile dal firmware con cine_h o wifi_h_ini a 2:
   *   - l'ora inesistente di marzo (02:00..02:59) finisce dopo la
   *     transizione, cioè alle 03:00 legali;
   *   - l'ora ripetuta di ottobre (02:00..02:59) si risolve sulla SECONDA
   *     occorrenza, quella in ora standard.
   */
  inline time_t fusoMktime(struct tm* t)
  {
    using namespace detail;
    int anno = t->tm_year + 1900;
    int mese = t->tm_mon;
    anno += mese / 12;
    mese %= 12;
    if (mese < 0) { mese += 12; --anno; }

    const int64_t giorni = giorniDaCivile(anno, mese + 1, 1) + (t->tm_mday - 1);
    const int64_t locale = giorni * 86400 +
                           (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 + t->tm_sec;

    int64_t utc;
    if (t->tm_isdst > 0)       utc = locale - offDst;
    else if (t->tm_isdst == 0) utc = locale - offStd;
    else
    {
      utc = locale - offStd;
      if (inDst(utc))
      {
        const int64_t alternativo = locale - offDst;
        if (inDst(alternativo)) utc = alternativo;
      }
    }

    const time_t ris = (time_t)utc;
    fusoLocaltime(&ris, t);
    return ris;
  }
}

using fuso::fusoImposta;
using fuso::fusoLocaltime;
using fuso::fusoMktime;
