---
name: Webapp e' un git submodule
description: c:\epd\webapp e' un git submodule separato puntato a un repo GitHub diverso da quello del firmware
type: reference
---

`c:\epd\webapp\` e' un git submodule (vedi `c:\epd\.gitmodules`):
- path: `webapp`
- url: `https://github.com/alesimattia/cinema-programmation-feed`

Il repo principale c:\epd e' il firmware ESP32; il submodule e' la webapp Python. Hanno storia git separata.

**How to apply:**
- Modifiche dentro `c:\epd\webapp\` vanno commitate nel repo del submodule (non in quello del firmware). Dal repo principale poi serve `git add webapp` per aggiornare il puntatore al commit.
- `git status` dal repo principale mostra il submodule come "modified content" se ci sono commit non riferenziati: e' normale, non un errore.
- Il `.gitignore` del submodule (`webapp/.gitignore`) e' diverso da quello principale (`c:\epd\.gitignore` ignora solo `Env.h` e `.DS_Store`).
- Pull request: separate per i due repo. Il deploy render.com punta al submodule.
