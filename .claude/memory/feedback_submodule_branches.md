---
name: Submodule senza branch di pannello
description: webapp, GxEPD2_SOLUM_ESL e routefinder lavorano solo su main; i branch dedicati a un pannello o a un layout esistono solo nel repo del firmware
type: feedback
---

I branch di lavoro legati a un pannello o a un layout, come quello di un redesign del 9.7", si creano solo nel repo del firmware `A:\epd`. I submodule `webapp`, `GxEPD2_SOLUM_ESL` e `routefinder` non hanno branch di questo tipo: i loro commit, compresi quelli che compattano più commit in uno, vanno su `main`.

**Why:** richiesta esplicita dell'utente ("submodule webapp non deve avere branch new_layout in quanto non è collegato ad uno specifico display", "lo squash deve essere nel main", "submodule gxepd2_solum non deve avere branch new_layout bensì la commit deve essere sul main"). I submodule sono progetti autonomi, il server cinema e la libreria dei driver, che servono entrambi i pannelli.

**How to apply:** quando una richiesta chiede un branch dedicato "nel repo e nei submodule", crearlo solo in `A:\epd` e nei submodule lavorare su `main`, chiedendo se il dubbio resta. Prima di un commit o di uno squash in un submodule verificare con `git branch -vv` di essere su `main`. Push, force push e cancellazione di branch remoti restano su autorizzazione esplicita. Vedi [[webapp_submodule]], [[gxepd2_solum_esl_library]], [[routefinder_submodule]].
