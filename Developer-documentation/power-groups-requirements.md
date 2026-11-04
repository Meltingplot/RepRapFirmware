# Leistungsgruppen: Anforderungen

Fassung 1 (Entwurf), 2026-11-03. Nichts davon ist gebaut. Alle Anforderungen haben Status E, alle offenen Punkte
O-01 bis O-10 sind entschieden (§4, §5).

**Zweck:** Eine Leistungsgruppe teilt ein Netzteil zwischen mehreren Heizern und virtuellen Verbrauchern, so dass
das Netzteil nie mehr als sein Budget liefern muss. Beispiel (Tim): zwei Heizer mit je 120 W und 80 W virtuelle
Verbraucher an einem 200-W-Netzteil.

**Geltungsbereich:** RRF auf dem Hauptboard, Duet3Expansion auf Tool- und Erweiterungsboards, CANlib für die
Nachrichten zwischen beiden. Dieses Dokument hält nur das Was fest; Entwurf, Kosten und Umsetzung folgen später.

## 0 IDs, Status, Nachweis

- **ID:** `R-BER-nnn`, Bereiche in §2. Eine Nummer wird nie wieder vergeben; eine gestrichene Anforderung bleibt mit
  „entfällt (Datum, Grund)“ stehen. Offene Punkte: `O-nn` (§5).
- **Status (St.):** **E** entschieden durch Tim (Datum in der Quelle). **V** Vorschlag dieses Dokuments, offen bis zur
  Bestätigung durch Tim.
- **Nachweis (Nw.):** **P** Prüfung (Funktionstest), **M** Messung, **A** Analyse, **R** Review.

## 1 Begriffe

| Begriff | Bedeutung |
|---|---|
| Leistungsgruppe | Heizer und virtuelle Verbraucher, die sich ein Budget teilen |
| Budget | Leistung in W, die die Gruppe zu keinem Zeitpunkt überschreiten darf |
| Nennleistung | Leistung, mit der ein Heizer bei eingeschaltetem Ausgang im Budget zählt (R-BUD-003) |
| Virtueller Verbraucher | Last, die RRF nicht als Heizer schaltet, die aber am selben Netzteil hängt |
| Zeitfenster | Zeitraum in der synchronen Step-Clock, in dem ein Heizer eingeschaltet sein darf |
| Engpass | Die Heizer brauchen zusammen mehr, als das Budget nach Abzug der virtuellen Verbraucher hergibt |

## 2 Anforderungen

### 2.1 GEN: Allgemeines

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-GEN-001 | Eine Gruppe kann Heizer auf dem Hauptboard und auf CAN-Boards (Tool- und Erweiterungsboards) gemeinsam enthalten. | Tim, 2026-11-03 | E | P |
| R-GEN-002 | Die Grenze aus `M307 … S` (maximale PWM) gilt in einer Gruppe weiter, zusätzlich zum Budget, auch für den Tastgrad im Fenster. Im Budget zählt der Heizer trotzdem mit seiner vollen Leistung (R-BUD-003), weil S nur den Mittelwert senkt. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-GEN-003 | Heizer ohne Gruppe verhalten sich wie heute: keine Zeitfenster, keine Abschaltung per Register, PID und Fehlerprüfungen unverändert. Gruppen-Heizer und freie Heizer können auf demselben Board nebeneinander laufen. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-GEN-004 | Das Feature lebt nur im Meltingplot-Fork, hinter einem Define; ist es aus, verhält sich die Firmware wie bei Duet3D. | Tim, 2026-11-03 (O-06) | E | R |
| R-GEN-005 | Die Nummer des M-Codes und die neuen CAN-Nachrichtentypen werden vor der Verwendung bei Duet3D angefragt bzw. reserviert. `CanMessageType` hat keinen Bereich für Dritte (`CANlib/src/CanId.h:16`). | Tim, 2026-11-03 (O-06) | E | R |

### 2.2 BUD: Budget

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-BUD-001 | Zu keinem Zeitpunkt überschreitet die Summe der Nennleistungen aller gleichzeitig eingeschalteten Heizer-Ausgänge einer Gruppe plus die Summe ihrer virtuellen Reservierungen das Budget. Es gilt der Spitzenwert, nicht der Mittelwert über eine PWM-Periode. | Tim, 2026-11-03 | E | M, A |
| R-BUD-002 | Übersteigen die virtuellen Reservierungen allein das Budget, lehnt der G-Code, der das verursacht, mit einem Fehler ab; die bisherige Reservierung bleibt bestehen. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-BUD-003 | Ein eingeschalteter Heizer zählt mit einem fest konfigurierten Wert in W: der höchsten Leistung, die vorkommt (kalt, bei der höchsten Netzteilspannung). Keine Korrektur über die gemessene Spannung. | Tim, 2026-11-03 (O-04) | E | R, M |

### 2.3 SLOT: Zeitfenster

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-SLOT-001 | Das Hauptboard legt für jeden Heizer einer Gruppe die Zeitfenster fest, in denen er eingeschaltet sein darf. Die Zeiten sind Zeitpunkte der synchronen Step-Clock. | Tim, 2026-11-03 | E | R, P |
| R-SLOT-002 | Die Zeitfenster gehen per CAN an die Boards und werden vorausgeplant wie Bewegungsbefehle. | Tim, 2026-11-03 | E | R, P |
| R-SLOT-003 | Außerhalb seines Zeitfensters ist ein Heizer-Ausgang per Register abgeschaltet (PWM-Freigabe), sofort und nicht erst über einen Tastgrad von 0 (Randbedingung B3). | Tim, 2026-11-03 | E | M |
| R-SLOT-004 | Das Zeitfenster ist nur die Obergrenze. Innerhalb des Fensters regelt die PID auf dem Board des Heizers wie heute mit Hardware-PWM; der Tastgrad wird auf die Fensterlänge hochgerechnet. Der Regelkreis bleibt lokal: CAN-Laufzeit und Vorlauf der Fenster gehen nicht in die Regelung ein. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-SLOT-005 | Zwei Fenster, deren Heizer zusammen das Budget überschreiten würden, überlappen nicht, auch nicht für Mikrosekunden. Ein Abstand zwischen ihnen deckt die Synchronisationsabweichung der Boards (Randbedingung B4) und die Schaltzeit der Ausgänge ab. | Tim, 2026-11-03 (bestätigt) | E | M, A |
| R-SLOT-006 | Zur Schaltzeit nach R-SLOT-005 gehört die Verzögerung des Stellglieds. Beispiel: Ein Bett über ein SSR mit Nulldurchgang an 50 Hz leitet nach dem Abschalten des Pins bis zur nächsten Strom-Nullstelle weiter, also bis zu 10 ms. Erst danach darf ein Heizer einschalten, der mit ihm zusammen das Budget überschreiten würde. | Tim, 2026-11-03 | E | M |
| R-SLOT-007 | Kein Fenster ist kürzer als eine Periode der PWM-Frequenz des Heizers (aus `M950 … Q`), damit der Tastgrad im Fenster stimmt (z. B. SSR-Bett mit langsamer PWM). Reicht der Platz in einem Zyklus dafür nicht, bekommt der Heizer in diesem Zyklus kein Fenster. | Tim, 2026-11-03 | E | P, M |

### 2.4 PRIO: Verteilung bei Engpass

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-PRIO-001 | Jeder Heizer einer Gruppe hat eine Priorität. Bei Engpass werden Heizer höherer Priorität zuerst versorgt. | Tim, 2026-11-03 | E | P |
| R-PRIO-002 | Die übrigen Heizer teilen sich den Rest. Heizer gleicher Priorität bekommen gleiche Anteile; wer weniger braucht, bekommt nur seinen Bedarf, und sein Überschuss geht zu gleichen Teilen an die anderen (max-min-fair). | Tim, 2026-11-03 (O-02) | E | P, A |
| R-PRIO-003 | Ein Heizer, der nicht ins Budget passt, bleibt abgeschaltet. | Tim, 2026-11-03 | E | P |
| R-PRIO-004 | Ein Heizer niedriger Priorität, der schon länger keine Leistung bekommen hat, steigt in der Priorität (Schutz gegen Aushungern). Einzelheiten: O-07 bis O-10. | Tim, 2026-11-03 | E | P |
| R-PRIO-005 | Die Wartezeit nach R-PRIO-004 zählt nur, solange der Heizer Bedarf hat und gar kein Fenster bekommt. Ein Heizer, der wenigstens einen Teil bekommt oder nichts braucht (auf Temperatur, aus), steigt nicht auf. | Tim, 2026-11-03 (O-07) | E | P |
| R-PRIO-006 | Ein ausgehungerter Heizer steigt höchstens bis zur höchsten Priorität der Gruppe und teilt sich dann das Budget gleich mit diesen Heizern (R-PRIO-002). | Tim, 2026-11-03 (O-08) | E | P |
| R-PRIO-007 | Der Aufstieg geht stufenweise: je abgelaufener Wartezeit eine Priorität höher, so dass die Reihenfolge der übrigen Prioritäten erhalten bleibt. Die Wartezeit wird je Gruppe im Gruppen-M-Code eingestellt. | Tim, 2026-11-03 (O-09) | E | P |
| R-PRIO-008 | Ein aufgestiegener Heizer bleibt oben, bis er in einem Zyklus seinen vollen Bedarf bekommt; dann fällt er sofort auf seine konfigurierte Priorität zurück. | Tim, 2026-11-03 (O-10) | E | P |

### 2.5 VIRT: Virtuelle Verbraucher

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-VIRT-001 | Virtuelle Verbraucher gehen mit ihrer Leistung ins Budget ein. | Tim, 2026-11-03 | E | P |
| R-VIRT-002 | Eine Reservierung wird zur Laufzeit per G-Code gesetzt, geändert und gelöscht. Ein fester Wert wird einmal in `config.g` gesetzt. | Tim, 2026-11-03 (O-01) | E | P |
| R-VIRT-003 | Eine Erhöhung gilt erst, wenn sie auf allen Boards der Gruppe wirkt; der Befehl kehrt erst dann zurück, so dass die nächste Zeile die echte Last einschalten darf. | Tim, 2026-11-03 (O-01) | E | M, P |

### 2.6 FR: Fehlerreaktion

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-FR-001 | Ein Gruppen-Heizer heizt nur in Zeitfenstern, die ihm bereits zugeteilt sind; endet das letzte, bleibt er abgeschaltet. Es gibt keine eigene Frist: Ohne neue CAN-Nachrichten laufen die Fenster zwangsläufig aus. | Tim, 2026-11-03 (O-05) | E | P |
| R-FR-002 | Ein Heizer auf einem Board, dessen Firmware keine Zeitfenster kann, wird in keine Gruppe aufgenommen; die Gruppenkonfiguration wird mit einem Fehler abgelehnt, und der Heizer bleibt frei (R-GEN-003). Sonst heizt er ohne Fenster. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-FR-003 | Die Aufheizprüfung („rising too slowly“) eines Gruppen-Heizers erwartet nur den Temperaturanstieg, den die tatsächlich gelieferte Leistung ergibt. Ein Heizer, den das Budget bremst, löst keinen Fehlalarm aus (Randbedingung B6). | Tim, 2026-11-03 (O-03) | E | P |
| R-FR-004 | Autotuning (M303) eines Gruppen-Heizers startet nur, wenn seine Leistung plus die virtuellen Reservierungen ins Budget passen; sonst wird M303 mit Fehler abgelehnt. Der Heizer bekommt dann für die ganze Tuning-Dauer ein durchgehendes Fenster vor allen Prioritäten, die anderen Heizer teilen sich den Rest (Randbedingung B7). | Tim, 2026-11-03 (bestätigt) | E | P |
| R-FR-005 | Die Abweichungsprüfung bei Solltemperatur bleibt unverändert. Kann das Budget einen Heizer nicht auf Solltemperatur halten, löst das einen Heizerfehler aus. | Tim, 2026-11-03 (O-03) | E | P |
| R-FR-006 | Die Übertemperatur-Wächter (M143, mit ihrer Aktion A) bleiben für Gruppen-Heizer unverändert aktiv und schalten auch innerhalb eines Fensters ab. Die Gruppe kann einen Heizer nur zusätzlich abschalten, nie einschalten oder einen Wächter umgehen. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-FR-007 | Ein Board, dessen Step-Clock nicht synchron ist (nach dem Start, während einer Neusynchronisation), hält seine Gruppen-Heizer abgeschaltet, auch innerhalb zugeteilter Fenster, bis die Step-Clock wieder synchron ist. | Tim, 2026-11-03 (bestätigt) | E | P |

### 2.7 CFG: Konfiguration und Anzeige

| ID | Anforderung | Quelle | St. | Nw. |
|---|---|---|---|---|
| R-CFG-001 | Ein neuer M-Code konfiguriert alles zur Gruppe: Budget, Heizer mit Nennleistung und Priorität, virtuelle Verbraucher, Bericht. M950 und M307 bleiben unverändert. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-CFG-002 | Budget, reservierte, zugeteilte und angeforderte Leistung jeder Gruppe stehen im Object Model (z. B. `heat.powerGroups[]`) und im Bericht des M-Codes ohne Parameter, damit Makros, Daemon und DWC sehen, ob ein Heizer gebremst wird und wie viel frei ist. | Tim, 2026-11-03 (bestätigt) | E | P |
| R-CFG-003 | Jeder Heizer einer Gruppe hat eine konfigurierbare Abschaltverzögerung (Standard 0 für MOSFET, z. B. 10 ms für ein SSR mit Nulldurchgang an 50 Hz). Der längere Abstand nach R-SLOT-006 entsteht nur nach Fenstern dieses Heizers. | Tim, 2026-11-03 | E | P, M |

## 3 Randbedingungen aus dem Bestand (Stand 2026-11-03)

| ID | Befund | Fundstelle |
|---|---|---|
| B1 | Heizer-Ausgänge sind freilaufende Hardware-PWM, Standard 250 Hz, ohne Phasenbezug zwischen den Kanälen. Ein Budget auf das Tastverhältnis hält nur den Mittelwert. | `CANlib/src/RRF3Common.h:97`, `ReprapFirmware/src/Config/Pins_Duet3_MB6HC.h:312, 321, 329` |
| B2 | Der Heater-Task läuft alle 250 ms und schreibt die PWM jedes Heizers sofort. | `RRF3Common.h:61`, `ReprapFirmware/src/Heating/Heat.cpp:410-420`, `LocalHeater.cpp:579` |
| B3 | Ein neuer Tastgrad wirkt erst am Ende der laufenden PWM-Periode (bis 4 ms bei 250 Hz). | `CoreN2G/src/SAM4S_4E_E70/AnalogOut.cpp:117`, `CoreN2G/src/SAME5x_C21/AnalogOut.cpp:90, 158` |
| B4 | Die Step-Clock der Erweiterungsboards ist per CAN auf das Hauptboard geregelt; gemessen am SZP ±4 Ticks (≈ ±5 µs); ab 10 ms Abweichung neu synchronisiert. | `Duet3Expansion/src/Movement/StepTimer.cpp:204-260`, `StepTimer.h:110, 148`, `m122-b60-frozen.txt` |
| B5 | Ein Erweiterungsboard meldet je Heizer nur einen 5-s-Mittelwert der PWM; die Statusnachricht ist mit 62 von 64 Byte voll. | `CANlib/src/CanMessageFormats.h:871-890`, `RRF3Common.h:62` |
| B6 | „Rising too slowly“ rechnet mit der tatsächlich ausgegebenen PWM; bei kleiner PWM wird die erwartete Heizrate negativ. Bei Solltemperatur löst eine Abweichung nach der M570-Zeit einen Fehler aus. | `LocalHeater.cpp:415-435, 615`, `CANlib/src/HeaterModel.cpp:45` |
| B7 | Autotuning heizt mit konstanter PWM. | `ReprapFirmware/src/Heating/Heater.cpp:315` |
| B8 | CHX 350: Bett H0 auf `out7` (Hauptboard), Düse H1 auf `20.out0` (TOOL1LC). | `chx350-config` `sys/config.g:119, 127` |

## 4 Entscheidungen

- 2026-11-03 (Tim): Spitzenwert statt Mittelwert (R-BUD-001).
- 2026-11-03 (Tim): Zeitfenster in der synchronen Step-Clock, per CAN wie Bewegungsbefehle, außerhalb des Fensters
  per Register aus (R-SLOT-001 bis -003); „klappt lokal sowie auch remote“ (R-GEN-001).
- 2026-11-03 (Tim): Bei Engpass höhere Priorität zuerst, die anderen teilen sich den Rest, was nicht passt, bleibt
  aus (R-PRIO-001 bis -003).
- 2026-11-03 (Tim, O-01): virtuelle Verbraucher per G-Code; eine Erhöhung gilt erst, wenn sie auf allen Boards
  wirkt (R-VIRT-002, R-VIRT-003).
- 2026-11-03 (Tim, O-02): gleiche Anteile bis zum Bedarf, Überschuss gleich verteilt (R-PRIO-002).
- 2026-11-03 (Tim, O-03): Aufheizprüfung mit gelieferter Leistung, Abweichungsprüfung unverändert (R-FR-003,
  R-FR-005).
- 2026-11-03 (Tim, O-04): fester Wert, die höchste vorkommende Leistung (R-BUD-003).
- 2026-11-03 (Tim, O-05): keine eigene Frist; ein Heizer heizt nur in bereits zugeteilten Fenstern, die ohne neue
  Nachrichten von selbst auslaufen (R-FR-001).
- 2026-11-03 (Tim, O-06): nur im Fork hinter einem Define; M-Code und CAN-Nachrichtentypen mit Duet3D abstimmen
  (R-GEN-004, R-GEN-005).
- 2026-11-03 (Tim): alle Vorschläge bestätigt (R-GEN-002, -003, R-BUD-002, R-SLOT-004, -005, R-FR-002, -004,
  -006, -007, R-CFG-001, -002); im Fenster normale PWM mit lokalem Regelkreis, strenger Abstand ohne Überlappung,
  ein neuer M-Code für alles.
- 2026-11-03 (Tim, SSR-Hinweis): Abschaltverzögerung des Stellglieds (SSR mit Nulldurchgang bis 10 ms an 50 Hz)
  gehört zum Abstand und wird je Heizer konfiguriert; kein Fenster kürzer als eine PWM-Periode (R-SLOT-006,
  R-SLOT-007, R-CFG-003).
- 2026-11-03 (Tim): Schutz gegen Aushungern (R-PRIO-004). Zählt nur ohne Fenster trotz Bedarf (O-07), steigt
  höchstens bis zur höchsten Priorität der Gruppe (O-08), stufenweise mit einer Wartezeit je Gruppe (O-09), fällt
  zurück, sobald sein Bedarf gedeckt ist (O-10) (R-PRIO-005 bis -008).

## 5 Offene Punkte (Interview)

| ID | Frage | Vorschlag |
|---|---|---|
| O-01 | Virtuelle Verbraucher: per G-Code gesetzt, fest aus der Konfiguration, oder an einen Ausgang gekoppelt? | erledigt 2026-11-03: per G-Code (R-VIRT-002, -003) |
| O-02 | „Den Rest teilen“: zu gleichen Teilen, nach Bedarf oder proportional? | erledigt 2026-11-03: gleich bis gedeckt (R-PRIO-002) |
| O-03 | Fehlerverhalten eines unterversorgten Heizers | erledigt 2026-11-03: mit gelieferter Leistung (R-FR-003, -005) |
| O-04 | Nennleistung als fester Wert oder mit Korrektur über die gemessene Versorgungsspannung (P ∝ V²)? | erledigt 2026-11-03: fester Wert (R-BUD-003) |
| O-05 | Wie viele ausgefallene Fensternachrichten ein Board überbrücken darf, bevor R-FR-001 greift | erledigt 2026-11-03: keine Frist, die Fenster laufen aus (R-FR-001) |
| O-06 | Nur im Fork hinter einem Define, oder als Beitrag für Duet3D? | erledigt 2026-11-03: Fork + Define, Nummern mit Duet3D abstimmen (R-GEN-004, -005) |
| O-07 | R-PRIO-004: Was zählt als „keine Leistung bekommen“? | erledigt 2026-11-03: kein Fenster trotz Bedarf (R-PRIO-005) |
| O-08 | R-PRIO-004: Wie hoch darf er steigen? | erledigt 2026-11-03: bis zur höchsten der Gruppe (R-PRIO-006) |
| O-09 | R-PRIO-004: Wie schnell steigt er? | erledigt 2026-11-03: stufenweise, Wartezeit je Gruppe (R-PRIO-007) |
| O-10 | R-PRIO-004: Wann fällt er auf seine konfigurierte Priorität zurück? | erledigt 2026-11-03: wenn sein Bedarf gedeckt ist (R-PRIO-008) |
