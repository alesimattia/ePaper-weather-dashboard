---
name: routefinder è un git submodule
description: A:\epd\routefinder punta a github.com/alesimattia/RouteFinder, branch main; servizio FastAPI su render.com, venv solo in routefinder\.venv, regole in routefinder\CLAUDE.md
metadata:
  type: reference
---

`A:\epd\routefinder\` è un git submodule (`.gitmodules`: url `https://github.com/alesimattia/RouteFinder`, `branch = main`), con storia separata dal firmware. Servizio FastAPI per render.com free tier, gemello della webapp cinema ([[webapp_submodule]]): Google Routes API + Maps Static API, risposta JPEG o piani 1bpp. Architettura, contratti col firmware e vincoli Google stanno in `routefinder\CLAUDE.md`; la memoria propria del servizio in `routefinder\.claude\memory\`.

**How to apply:**
- Commit nel repository del submodule, poi nel padre si aggiorna il puntatore; la commit del submodule va pushata prima di committare il padre. Solo `main` ([[feedback_submodule_branches]]).
- Il venv sta solo in `routefinder\.venv` ([[feedback_routefinder_venv]]).
- Le credenziali GitHub dei remote non stanno nei `.git/config`: le aggiunge una regola `url.<...>.insteadOf` della configurazione globale, inclusa da `A:\Git-config-local\.gitconfig-personal-auth`, quindi `git remote -v` mostra l'URL già riscritto. Mai copiare quell'URL in `.gitmodules` o in un remote.
- Il firmware lo usa tramite [[routefinder_morning_route]].
