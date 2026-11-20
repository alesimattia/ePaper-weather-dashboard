---
name: feedback-comment-measures
description: Nei commenti del firmware niente misure in pixel che dipendono dal layout; i commenti restano perchè identificano i pezzi del layout
metadata:
  type: feedback
---

Nei commenti del firmware non si scrivono misure che dipendono dal layout (larghezze, altezze, coordinate, conteggi di righe derivati dalle costanti di `Layout_common.h` o dei `Layout_<pannello>.h`): si descrive la relazione o il vincolo a parole e si rimanda alla costante. I commenti non si eliminano, perchè servono a identificare e "chiamare" un pezzo del layout.

**Why:** le misure cambiano quando cambia il layout e il commento resta falso senza che nessuno se ne accorga.

**How to apply:** restano ammesse le misure che non dipendono dal layout, come le metriche dei font (cap height, larghezza di "-12" in 9pt), i fatti del pannello (960w, 384 righe per controller) e il formato delle bitmap scritte a mano. Le misure del layout stanno in README.md e in [[layout_invariants]], che si aggiornano insieme al codice.
