---
name: Venv di routefinder solo nel suo repository
description: il virtualenv Python di routefinder sta esclusivamente in A:\epd\routefinder\.venv, non in A:\tmp\sandbox nè altrove
metadata:
  type: feedback
---

Il virtualenv di `routefinder` si crea e si usa solo in `A:\epd\routefinder\.venv` (gitignored), da `C:\Python314`. Fa eccezione alla regola generale "installazioni solo in `A:\tmp\sandbox`" di `A:\epd\CLAUDE.md`.

**Why:** indicazione esplicita dell'utente ("il venv deve essere salvato esclusivamente dentro A:\epd\routefinder").

**How to apply:** test con `routefinder\.venv\Scripts\python -m pytest`; un venv non si sposta (gli script hanno percorsi assoluti), quindi se manca si ricrea lì con `requirements-dev.txt`. Vedi [[routefinder_submodule]].
