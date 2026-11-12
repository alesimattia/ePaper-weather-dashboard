#!/bin/bash
# Compila ed esegue i test su host. Nessuna board coinvolta: verificano la
# logica pura di Timings.h, Scheduler.h e del refresh differenziale
# (PartialRefresh.h e la sua macchina a stati), che sul dispositivo non e'
# osservabile senza aspettare ore.
#
#   ./test/esegui.sh
#
# Gli eseguibili finiscono in una cartella temporanea fuori dal repo: qui non
# devono restare artefatti di build.
set -u
cd "$(dirname "$0")"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# -I stub PRIMA di -I..: gli stub coprono la piattaforma (Arduino,
# Preferences, WiFi, esp_sleep), mentre Timings.h e Scheduler.h sono quelli
# VERI del firmware. Testare una copia non direbbe niente.
FLAGS="-std=gnu++20 -Istub -I.. -Wall -Wextra -Wno-format-security -Wno-unused-parameter"

falliti=0
for caso in test_timings test_scheduler test_routefinder test_tuya test_tuya:-DTUYA_REQUEST_ACK=0 test_partial test_partial:-DDISPLAY_PARTIAL_REFRESH=1; do
  # Un caso è "nome" oppure "nome:opzioni del compilatore": test_tuya gira due
  # volte, con e senza la conferma applicativa del cloud, e test_partial con
  # l'interruttore del differenziale spento e acceso.
  t=${caso%%:*}
  extra=""
  [ "$t" != "$caso" ] && extra=${caso#*:}
  echo "── $caso ─────────────────────────────────────────────────────────"
  if ! g++ $FLAGS $extra -o "$OUT/$t" "$t.cpp"; then
    echo "compilazione FALLITA"; falliti=$((falliti+1)); continue
  fi
  "$OUT/$t" || falliti=$((falliti+1))
  echo
done

if [ $falliti -ne 0 ]; then
  echo "$falliti test con fallimenti"
  exit 1
fi
echo "tutti i test superati"
