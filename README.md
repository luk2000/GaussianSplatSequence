# Gaussian Splat Sequence – Unreal Engine 5.7 → COLMAP → LichtFeld Studio

Ein Editor-Plugin für Unreal Engine 5.7. Es macht aus einer gerenderten Kamerafahrt eine **Sequenz von Gaussian Splats**, eine pro Frame. Die Idee ist ähnlich wie bei Apple SHARP (Splat aus einem einzelnen Bild), nur dass hier **Ground-Truth-Depth** aus Unreal statt geschätzter Tiefe verwendet wird.

```
Unreal (Sequencer-Kamera) ──► COLMAP cameras/images ─┐
Movie Render Queue/Graph  ──► Beauty PNG + Depth EXR ─┼─► frame_XXXX/{images, sparse/0} ──► LichtFeld Studio ──► frame_XXXX.ply
                                                      └─► points3D (kongruent zur Kamera)
```

## Features

| | |
|---|---|
| **Kamera-Export** | Aktuelle Kamera (ausgewählter Camera/CineCamera-Actor oder Viewport inkl. Pilot/Camera-Cuts) als COLMAP `PINHOLE` (`cameras.txt/.bin`, `images.txt/.bin`). |
| **Sequenz-Export** | Geht die offene Level Sequence Frame für Frame durch (Playback-Range oder eigener Bereich) und schreibt pro Frame einen eigenständigen COLMAP-Datensatz. |
| **Depth → Punktwolke** | Liest den Depth-EXR (planar/SceneDepth oder radial), projiziert jeden Pixel mit **genau der exportierten Kamera** zurück und schreibt `points3D.txt/.bin` + `points.ply`. Die Punkte werden aus dem Beauty-Pass eingefärbt, fliegende Kantenpixel werden gefiltert. |
| **Training** | Erzeugt `train_lichtfeld.bat/.sh` (ein Splat pro Frame, optional initialisiert mit dem Splat des Vorframes) und startet es auf Wunsch direkt. |
| **Python/Blueprint** | Alle Schritte gibt es als `UGSSBlueprintLibrary`, also in Blueprints und im Editor-Python (`unreal.GSSBlueprintLibrary`). |
| **CLI-Tool** | `Tools/gss_tools.py`: Batch-Konvertierung außerhalb von Unreal (auch **Multilayer-EXR**), Kongruenz-Check und Training. |

## Installation

1. Repository nach `<DeinProjekt>/Plugins/GaussianSplatSequence` klonen.
2. Projekt-`.uproject` neu öffnen, die Frage nach dem Kompilieren mit Ja beantworten (C++-Toolchain für UE 5.7 nötig). Alternativ: Rechtsklick auf `.uproject` → *Generate Visual Studio project files* → bauen.
3. **Window → Gaussian Splat Sequence** öffnen.

Abhängigkeiten sind nur Engine-Module und das (standardmäßig aktive) Plugin *LevelSequenceEditor*.

## Workflow

### 1. Kameras exportieren
* **Output Directory** setzen, z. B. `D:/Splats/Shot010`.
* **Image Width/Height** auf die Render-Auflösung stellen.
* Kamera wählen:
  * *Selected Camera Actor*: Shot-Kamera im Outliner auswählen (bei Spawnables, während die Sequence offen ist).
  * *Level Viewport*: nimmt die Kamera, die der Viewport pilotiert bzw. über „Lock to Camera Cuts“ zeigt, sonst die freie Viewport-Kamera.
* **Export Current Camera** exportiert den aktuellen Sequencer-Frame. **Export Camera Sequence** exportiert alle Frames.

Ergebnis pro Frame:
```
Shot010/frame_0101/
  images/                 (Beauty-Bild kommt in Schritt 2 hinein)
  sparse/0/cameras.txt|bin, images.txt|bin, points3D.txt|bin
  ue_camera.txt           (Original-UE-Transform, FOV, unit_scale – zum Debuggen)
```

### 2. Rendern (Movie Render Queue oder Movie Render Graph)
* Gleiche Sequence, gleiche Kamera, **gleiche Auflösung**.
* **Beauty**: PNG (oder EXR, wird beim Konvertieren nach PNG gewandelt).
* **Depth**: zusätzlicher Pass, der *SceneDepth* in Unreal-Einheiten (cm) ausgibt. Bei MRQ z. B. *Deferred Rendering → Additional Post Process Materials →* `MovieRenderQueue_WorldDepth`, oder ein eigenes Post-Process-Material mit `SceneDepth` → Emissive.
  * EXR **32 bit** verwenden (Half-Float wird ab ~650 m ungenau).
  * Bei EXR-Output **Multilayer ausschalten**, damit jeder Pass eine eigene Datei bekommt. Multilayer-EXRs kann das Python-Tool lesen.
  * Anti-Aliasing mittelt die Tiefe an Objektkanten. Den Rest erledigt der *Edge Threshold*-Filter.
* Dateinamen-Pattern im Panel eintragen, `{frame}` wird mit *Frame Padding* aufgefüllt:
  * `Color Image Pattern`: `D:/Renders/Shot010/Shot010.FinalImage.{frame}.png`
  * `Depth EXR Pattern`: `D:/Renders/Shot010/Shot010.MovieRenderQueue_WorldDepth.{frame}.exr`
  * Weicht die Frame-Nummer im Dateinamen von der Sequencer-Frame-Nummer ab, das mit *Render Frame Offset* ausgleichen.

### 3. Depth → Punktwolke
**Convert Depth For All Frames**. Die Kamera wird aus den geschriebenen COLMAP-Dateien zurückgelesen. Kamera und Punktwolke gehen also durch exakt dieselbe Mathematik (`GSSMath.h`) und sind deshalb kongruent. Nützliche Einstellungen:

| Setting | Default | Bedeutung |
|---|---|---|
| Depth Type | Planar | Unreal SceneDepth ist planar (Abstand entlang der Blickachse). *Radial* für echte Distanz zur Kamera. |
| Depth Channel | R | Kanal im EXR. |
| Depth To Unreal Units | 1 | 100, falls die Tiefe in Metern gespeichert ist. |
| Min/Max Depth | 1 / 100000 cm | Himmel und zu nahe Pixel verwerfen. |
| Pixel Stride | 2 | Nur jeden n-ten Pixel verwenden (1920×1080, Stride 2 ≈ 520k Punkte). |
| Max Points | 0 | Obergrenze Punkte pro Frame (0 = aus). Überzählige Punkte werden zufällig-gleichmäßig ausgedünnt. Mit Stride 1 + Max Points bekommst du ein festes Punktbudget, unabhängig von der EXR-Auflösung. |
| Edge Threshold | 0.05 | Pixel mit > 5 % Tiefensprung zum Nachbarn verwerfen (fliegende Punkte). |

### 4. Training in LichtFeld Studio
* **LichtFeld Executable** setzen, **Start Training** klicken (oder **Write Training Script** und das Skript selbst ausführen).
* Standard-Argumente pro Frame (im Panel frei editierbar):
  ```
  -d "{data}" -o "{output}" -i {iter} --headless --output-name {name} --export ply
  ```
* **Max Splats** begrenzt die Anzahl der Gaussians pro Frame (`--max-cap`). Dafür steht **Strategy** standardmäßig auf `mcmc`.
* **Init From Previous Frame** hängt `--init <Splat des Vorframes>` an. Das stabilisiert die Sequenz zeitlich und das Training konvergiert schneller. Ob der Pfad aus *Previous Splat Pattern* existiert, prüft das Skript. Wird der Splat nicht gefunden, trainiert es ohne Init. (Die Python-Variante sucht zusätzlich nach der neuesten `.ply` im Output.)
* Ergebnis: `Shot010/trained/frame_0101/frame_0101.ply` usw.

Einzelnen Frame manuell in der GUI öffnen: in LichtFeld Studio den Ordner `frame_0101` als COLMAP-Datensatz laden.

## Koordinatensysteme

* Unreal: linkshändig, X vorne, Y rechts, Z oben, Zentimeter.
* COLMAP: rechtshändig. Kamera schaut entlang +Z, +X rechts, +Y unten. `images.txt` speichert World→Camera (`qw qx qy qz tx ty tz`), die Mitte des ersten Pixels liegt bei (0.5, 0.5).
* **World Axes** (Export):
  * *COLMAP/OpenCV (Y down)* (Standard): `x = UE.Y, y = −UE.Z, z = UE.X`
  * *Right-handed Z up*: `x = UE.X, y = −UE.Y, z = UE.Z`
  * *Right-handed Y up*: `x = UE.Y, y = UE.Z, z = −UE.X`
* **Unit Scale** 0.01 = cm → m. Gilt für Kamera und Tiefe gleichermaßen.
* Intrinsics: `fx = fy = (W/2) / tan(HFOV/2)`, `cx = W/2`, `cy = H/2`. Unreal hält das horizontale FOV (Standard *Maintain X-Axis FOV*). Bei CineCameras wird es direkt aus Filmback und Brennweite berechnet. Das Filmback-Seitenverhältnis sollte zur Render-Auflösung passen, das Plugin warnt sonst.
* LichtFeld-Option `--centralize` bleibt aus (Default `off`). So bleiben alle Frames im selben Weltkoordinatensystem und die Splats einer Sequenz liegen deckungsgleich übereinander.

## Python-Tool (ohne Unreal)

```bash
pip install numpy OpenEXR pillow

# Depth -> Punkte für alle Frames (auch Multilayer: --depth-channel "FinalImage.WorldDepth.R"), max. 300k Punkte
python Tools/gss_tools.py convert --root D:/Splats/Shot010 \
    --depth "D:/Renders/Shot010/Shot010.MovieRenderQueue_WorldDepth.{frame}.exr" \
    --color "D:/Renders/Shot010/Shot010.FinalImage.{frame}.png" --stride 1 --max-points 300000

# Kongruenz prüfen (Punkte in die Kamera zurückprojizieren und mit der Tiefe vergleichen)
python Tools/gss_tools.py check --frame D:/Splats/Shot010/frame_0101 \
    --depth D:/Renders/Shot010/Shot010.MovieRenderQueue_WorldDepth.0101.exr

# Alle Frames trainieren
python Tools/gss_tools.py train --root D:/Splats/Shot010 --exe "C:/LichtFeld/LichtFeld-Studio.exe" \
    --iter 7000 --init-from-previous --max-splats 500000
```

Editor-Python in Unreal:
```python
import unreal
lib = unreal.GSSBlueprintLibrary
print(lib.export_camera_sequence())
print(lib.convert_depth_for_all_frames())
print(lib.launch_training())
```

## Tests

```bash
# Kamera-Mathematik (reines C++, kein Unreal nötig)
g++ -std=c++17 -O2 -ISource/GaussianSplatSequence/Public Tests/test_gss_math.cpp -o test_gss_math && ./test_gss_math

# End-to-End: synthetische Szene in Unreal-Konventionen raycasten -> EXR -> Punktwolke -> Geometrie prüfen
python Tests/test_gss_tools.py
```

## Hinweise und Grenzen

* **Eine Kamera pro Frame**: Aus genau einer Ansicht trainiert, sieht der Splat aus der Originalperspektive perfekt aus. Bei größeren Blickwinkeländerungen entstehen Lücken (verdeckte Bereiche), genau wie bei SHARP. Die Ground-Truth-Punktwolke gibt die Geometrie aber exakt vor. Für mehr Blickwinkel-Robustheit kannst du zusätzliche Kameras rendern und deren Frames zusammenlegen.
* Linsenverzeichnung, Overscan und Lens-Shift werden nicht exportiert (PINHOLE-Modell).
* `points3D` hat leere Tracks (keine Feature-Matches). Das ist beim LichtFeld-Import in Ordnung, solange `--min-track-length` 0 bleibt (Default).
* Das Plugin ist gegen die UE-5.7-API geschrieben, konnte hier aber nicht in einem Unreal-Build kompiliert werden. Bei Build-Fehlern bitte ein Issue mit dem Compiler-Log aufmachen.

## Lizenz
MIT
