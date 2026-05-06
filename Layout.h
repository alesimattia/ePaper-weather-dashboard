#ifndef LAYOUT_H
#define LAYOUT_H

/**
 * Dispatcher del layout: include esattamente uno dei due Layout_*.h in
 * base al #define di selezione del display. I moduli applicativi
 * (Weather.h, Calendar.h, Graphics.h, .ino) includono SOLO questo header
 * e referenziano le costanti via `Layout::*`.
 *
 * Per cambiare display, scommentare nello sketch .ino il define
 * corrispondente PRIMA di qualsiasi #include che dipenda dal layout:
 *
 *   #define DISPLAY_VARIANT_097C
 *   //#define DISPLAY_VARIANT_122C
 *   #include "Layout.h"
 *
 * Esattamente uno deve essere definito.
 */

#if defined(DISPLAY_VARIANT_097C) && defined(DISPLAY_VARIANT_122C)
  #error "Layout: definire SOLO uno fra DISPLAY_VARIANT_097C e DISPLAY_VARIANT_122C"
#elif defined(DISPLAY_VARIANT_097C)
  #include "Layout_097c.h"
#elif defined(DISPLAY_VARIANT_122C)
  #include "Layout_122c.h"
#else
  #error "Layout: definire DISPLAY_VARIANT_097C oppure DISPLAY_VARIANT_122C nello sketch .ino prima di #include \"Layout.h\""
#endif

#endif
