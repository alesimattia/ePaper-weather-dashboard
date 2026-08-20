## Regole comportamentali per Claude
- Non aggiungere @since o @modified o "modificato"
- Non aggiungere Mattia Alesi
- Per i commenti che occupano una sola riga usa il tag // tranne che per i commenti sopra alle funzioni
- racchiudi tra i tag /** e */ i commenti che occupano più di una riga e i commenti sopra le funzioni
- se il blocco true di un if contiene una sola istruzione, allora non usare parentesi graffe e scrivi l'istruzione subito sotto ma indentata
- non essere prolisso nei commenti
- Non committare mai automaticamente
- ponimi altre domande se ci sono problemi o hai dubbi
- Non refactoring non richiesto: aggiusta solo quello che è stato chiesto
- Mostra sempre i percorsi file completi quando scrivi nella chat
- Per modifiche che toccano più di 3 file, presenta prima un piano
- Aggiungi sempre una breve descrizione sopra i metodi e le funzioni crei e scrivi anche i riferimenti alle classi che li usano se sono classi esterne al file corrente
- Quando modifichi delle funzioni o metodi già esistenti adegua il commento in modo che rifletta lo stato attuale del codice

## Memorie
Le memorie devono riflettere lo stato finale del codice, non stati di avanzamento nè modifiche. Descrivi cosa il codice fa nel presente e non come ci si è arrivati
evita quindi "fasi non fatte/da fare", date di modifica, "aggiornato", "rimosso" e simili formulazioni storiche o di progresso.