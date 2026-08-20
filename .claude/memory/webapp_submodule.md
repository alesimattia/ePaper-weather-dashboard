---
name: Webapp è un git submodule
description: A:\epd\webapp è un git submodule separato puntato a un repo GitHub diverso da quello del firmware
type: reference
---

`A:\epd\webapp\` è un git submodule (vedi `A:\epd\.gitmodules`):
- path: `webapp`
- url: `https://github.com/alesimattia/cinema-programmation-feed`

Il repo principale A:\epd è il firmware ESP32; il submodule è la webapp Python. Hanno storia git separata.

**How to apply:**
- Modifiche dentro `A:\epd\webapp\` vanno commitate nel repo del submodule (non in quello del firmware). Dal repo principale poi serve `git add webapp` per aggiornare il puntatore al commit.
- `git status` dal repo principale mostra il submodule come "modified content" se ci sono commit non riferenziati: è normale, non un errore.
- Il `.gitignore` del submodule (`webapp/.gitignore`) è diverso da quello principale, che ignora `Env.h`, `.DS_Store`, `**/.build` e `GxEPD2-master`.
- Pull request: separate per i due repo. Il deploy render.com punta al submodule.
