# Reverse Engineering: Game.exe 1.09 und Dateiformate

Alles hier gilt für **Game.exe v1.09**, MD5 `7dc4fda2f6b7e8a6b70e846686a81938`. Diese Datei ist für die deutsche und die englische Version identisch. Der Launcher prüft die MD5 vor jedem Start, deshalb dürfen die Hooks mit festen Adressen arbeiten. Jeder Patch prüft außerdem die Originalbytes, bevor er schreibt.

Werkzeuge: `tools/re/re_tools.py` (pefile + capstone), `tools/re/rfd.py` (Archive). Was alte Community-Tools über Karten, Musik/Sprache (BAG) und `rules.txt` verraten, steht in [`tools-and-formats.md`](tools-and-formats.md).

---

## Archive (RFH/RFD)

- Pro Thema `<PREFIX>0001.RFH/.RFD`, `<PREFIX>0002…`. Das Spiel lädt aufsteigend und **hört bei der ersten fehlenden Nummer auf**. Eigene Archive müssen deshalb die nächste freie Nummer bekommen. Vorhandene Archive stammen von den CDs oder vom Patch 1.09 (z. B. `MAPS0002`) und dürfen nie überschrieben werden.
- Kommt ein Eintrag doppelt vor, gewinnt der mit dem **neueren Zeitstempel**. Darüber lassen sich Originaldateien ersetzen.
- RFH, pro Eintrag (little-endian): `u32 name_len (inkl. NUL)`, `u32 timestamp`, `u32 flags (2 = zlib)`, `u32 stored_size (inkl. 4-Byte-Präfix)`, `u32 raw_size`, `u32 offset`, `char name[]`.
- RFD, an `offset`: `u32 raw_size`, danach der zlib-Stream.
- Präfixe im Spiel: 3DDATA (Modelle `*.xbf`, Animationen `*.xaf`, Texturen `Textures\*.tga`), UI (Menü-Szenen, `FONT\*.MAP`, `CAMERA.INI`), MAPS, MODEL, STRINGS, AI, MISSIONS, CAMPAIGN, SOUNDS.
- Texturen sind TGA mit 24 oder 32 Bit, selten 16 Bit ARGB1555. Magenta (255,0,255) dient als Farbschlüssel für Transparenz.

## Schriften (CFont3D)

- Font-Tabelle in `.rdata` ab `0x5d3540`, Datensätze zu 16 Byte: `char* name`, `u8 rowHeight (+4)`, `u8 (+5)`, `u32 pages (+8)`, `u8 (+0xc)`.
  - Index 0 „Small“: Zeilenhöhe 16, 2 Seiten (`small0.tga`, `small1.tga`). Damit werden Meldungen und Hinweise im Spiel gezeichnet.
  - Index 1 „Standard“: Zeilenhöhe 30, 1 Seite (`Standard.tga`). Damit werden Menüs, Buttons und der Ladebildschirm gezeichnet. **Das Spiel skaliert diese Schrift selbst mit der Auflösung.** Eine 2x-Version ist deshalb doppelt zu groß, das wurde bei 720p und 1440p getestet.
- Ladefunktion: `0x4ab330(mapPath, tgaPath, rowHeight, flag)`. Format-Strings `%s\%s%d.MAP` (mehrseitig) und `%s\%s.MAP`, Ordner „FONT“.
- `.MAP` = UTF-16 mit BOM, nur die Zeichenliste in Reihenfolge.
- Der Glyphen-Scanner läuft Zeilenband für Zeilenband (Höhe = rowHeight), **beginnt bei x = 1**, überspringt leere Spalten und nimmt eine zusammenhängende Folge nicht-leerer Spalten als Glyphe. Spalte 0 von Zeile 0 ist eine Markierung. Lücken innerhalb einer Glyphe (z. B. bei `"`) überbrückt man mit einem schwachen Pixel. Das Original nutzt dafür den Wert 16. Alle Werte sollten ≥ 16 sein, falls das Spiel eine 16-Bit-Kopie scannt.
- Leerzeichen = Breite von `_` mit leerem UV-Bereich bei (0,98–0,99). Diese Ecke der Seite muss leer bleiben.
- Emperor Spice: `tools/fonts/build_fontpack.py` rendert SmallHD/StandardHD aus DejaVu Sans und prüft das Layout gegen eine Nachbildung des Scanners. `EmperorHooks/LargeFont.cpp` biegt „Small“ auf „SmallHD“ mit Zeilenhöhe 32 um.

## Kamera (CAMERA.INI, UI-Archiv)

- Werte: `MinZoom 0.5` (am weitesten weg), `MaxZoom 1.25` (am nächsten), `StartZoom 0.35` usw.
- `0x532e5e: fdivr dword [0x5d0720]` mit 1.0f ⇒ `1/MinZoom`, gespeichert als größter Abstandsfaktor bei `[edi+0x30]`. MaxZoom landet analog bei `+0x34`.
- `EmperorHooks/CameraZoom.cpp` lässt den Operanden auf eine eigene Konstante k zeigen ⇒ der größte Abstand wird k/MinZoom. Getestet: 1,5 sieht gut aus, 2,0 ist zu weit.

## Geld / Skript-Interpreter

- Skript-Interpreter: Switch bei `0x4ef5d5`, Sprungtabelle `0x4f3fb4`, Index = Funktions-ID.
- Funktionstabelle `0x60a978`, Datensätze zu 92 Byte (`name[36]` + id). Ids: GetPlayerSide 14, GetSideCash 0x13, AddSideCash 0x3e, SetSideCash 0x98, Multiplayer 2.
- `[0x7d75d4]` Setup-Objekt; `0x5344e0(setup)` = Spieltyp (3 = Multiplayer); `0x5356a0(setup)` = Spielerseite.
- `[0x817c00]` Seiten-Manager (nur in einer Mission ≠ 0); `0x597bb0(sides, i)` = GetSide.
- `0x53ebf0(side, amount)` AddCash, `0x53ec10` SetCash; das Geld liegt bei `side+0x868`.

## Kartenboden (MAPS-Archiv, pro Karte ein Ordner)

- `map.inf`: WIDTH/HEIGHT in Zellen.
- `test.CPT`: „Wash“, also die Farb- und Übergangsschicht des Bodens. Header `u32 w=2048, u32 h=2048`, danach DXT3 in 64 Blöcken zu 256×256, angeordnet als Raster 8×8. Benutzt wird nur der Bereich der Karte. Das entspricht 8 Pixeln pro Zelle.
- `test.CPF`: 256×256 RGB565, Variante für niedrige Detailstufe.
- `texture.dat`: 2048×2048, 8 Bit, Schattenkarte.
- `test.xbf`: Geländenetz; `test.lit`: Licht.
- Laut Map-Editor-Handbuch sind die Gelände-Schatten vorgebacken (Shadowmaker), nur Gebäude und Einheiten werfen Echtzeit-Schatten. Pflichtdateien einer Karte: siehe tools-and-formats.md.
- **Lädt zuerst `test.tga`**, falls vorhanden („Loading TGA Wash“, Funktion `0x54f680`). Die Größe ist beliebig quadratisch, Blockgröße = Breite/8.
- Ergebnis eines Tests mit 2x per Real-ESRGAN: **im Spiel kaum sichtbar**. Der Wash ist nur eine weiche Farbschicht, die scharfen Risse und Krusten kommen von gekachelten Detailtexturen aus 3DDATA. Nicht weiterverfolgt.

## Icons im Seitenmenü

- `Textures\AT_*.tga`, `HK_*.tga`, `OR_*.tga` und `Grey_*` (nicht verfügbar), 64×64 mit 24 Bit, inklusive Scanline-Muster.
- Test mit 2x per Real-ESRGAN in einem eigenen 3DDATA-Archiv: wird geladen, ist aber **kaum besser**, weil die Vorlage zu klein ist. Wirklich besser würde es nur durch Neu-Rendern aus den 3D-Modellen. Das wäre ein großes Projekt, als Vorlage für einen XBF-Reader eignet sich CorrinoEngine.

## Grafik-Pipeline

- Das Spiel nutzt D3D7 über DirectDraw. dgVoodoo2 (`DDraw.dll`, `D3DImm.dll`) übersetzt das nach D3D11.
- MSAA in dgVoodoo **hängt das Spiel auf**. Supersampling über `[DirectX] Resolution = 2x` mit `Resampling = lanczos-3` ist der Test-Stand.
- ReShade wird von EmperorHooks per `LoadLibrary("ReShade32.dll")` aus dem data-Ordner geladen. Ein Proxy über dxgi/d3d11 funktioniert mit dgVoodoo nicht.
- Tiefe für ReShade: Im Gefecht passt `RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1.5`, nicht umgedreht, nicht kopfüber. Das Hauptmenü ist eine 3D-Szene, deren Tiefe Glamarye nicht verträgt (Bild schwarz). Deshalb schaltet `ReShadeAddon.cpp` solche Effekte ab, solange das Missions-Flag `0x817c0c` 0 ist.
- **Modell-Dateinamen `*_H0/H1/H2/M3/HC.xbf` sind keine Entfernungs-LODs, sondern Schadensstufen** (untersucht 13.11.):
  - Name wird bei `0x4f7c50` gebaut: `sprintf("%s%s_%c%c.xbf", …)`. Buchstabe = `"LMH"[[0x60f700]]`, Ziffer = `"C0123"[slot]`.
  - `[0x60f700]` = Spieloption **ModelLOD** (0 = Low, 1 = Medium, 2 = High), wird bei Missionsstart aus den Grafik-Einstellungen kopiert (`0x576745`). Standard und im Log des Testers: **H**, also schon die höchste Stufe. Fehlt eine Datei, probiert das Spiel die anderen Buchstaben (daher `M3`, ein `H3` gibt es nicht).
  - Slot-Auswahl bei `0x4fc7a0`: Verhältnis aktuelle/maximale Lebenspunkte. Über 0,66 (`[0x5d4408]`) → Slot 1 (`H0`, heil), über 0,33 (`[0x5d4404]`) → Slot 2 (`H1`), über 0 → Slot 3 (`H2`), sonst Slot 4 (`M3`, Ruine). Typen 0x35–0x37 → Slot 0 (`HC`, im Bau).
  - Ergebnis: Es gibt keine gröberen Modelle bei weitem Zoom. **„Höchste Detailstufe erzwingen“ bringt nichts**, das Spiel nutzt sie schon.

## Netzwerk (WOL-Emulation aus Emperor Reborn)

- IRC-ähnlicher Server auf TCP 4005, dazu ein UDP-Proxy (WolProxyServer/Client). Bei v1.1 abgesichert: unbekannte Befehle werden ignoriert, die Zahl der Verbindungen und die Puffer sind begrenzt, der Client akzeptiert nur Pakete vom eigenen Host.
- Wine/Proton: `OaEnablePerUserTLibRegistration` muss dynamisch geladen werden (PR #7).

## Archive loading and textures (found 15.11.2026, HD texture hang)
- `0x402f90` reads the RFH indexes `PREFIX0001.RFH`… with `fopen`/`fread` until the first gap; fields per entry go to a 0x20 struct (+4 ts, +8 flags, +0xC stored, +0x10 raw, +0x1C offset, +0x14 name), inserted via `0x4038b0`.
- `0x403b70` opens the RFDs. If the library flag `[this+0x97c]` is set, every RFD is read **completely into one malloc block** (`_stat` → `malloc(size)` → `fread`). Game.exe is 32 bit without Large Address Aware.
- `0x403490` `ResourceLibrary::Load(name, uint32* size, 0, 0)` (thiscall, `ret 0x10`) returns the file data. The texture loader at `0x54c2b8` builds `TEXTURES\name`, calls it and logs "Failed to find texture %s" on 0, then creates the texture with `0x417810(data, info)` (cdecl).
- Diagnostic hook: `EmperorHooks/TextureTrace.cpp` logs both calls until the first mission.

## Selection limit 64 (looked at 15.11.2026, not changed)
The player can select at most 64 units (box selection, reported by the project owner). In Game.exe 64 is not one
setting but the size of fixed lists: e.g. an array of 64 entries at `[obj+0x34]` (`0x4ba0c1`–`0x4ba102`,
`0x4bcb07`–`0x4bcb0d`), stack arrays of 64 (`0x4bb4ac`–`0x4bb4b7`), and about 50 more `cmp reg, 0x40` checks,
many in `0x4b7d32`–`0x4be829`. Raising the limit would mean moving these lists into bigger memory and changing
every access; orders in multiplayer and save games probably carry the same lists. Big and risky, not planned.

## XBF models: vertex animation (decoded 15.11.2026)
- Base reader: IceReaper/ebfd-re (`LibEmperor`, MIT); Python port in `tools/experiments/models/xbf.py`.
- Infantry (`Units\AT_inf_H0.xbf`): body object `LtINF` has only **60 vertices / 106 triangles**, animated per vertex (flag 4), 390 stored poses of 711. Gun, muzzle flash and selection marker are separate objects with object (matrix) animation.
- Vertex animation entry: 8 bytes per vertex and pose = 4 × int16. **Positions = first three int16 × 2^-unk** (`unk` from the animation header, 9 for the infantry = /512); pose 0 matches the static vertices exactly. The 4th int16 is a packed normal (decoded 15.11.2026, no table in Game.exe needed):
  - Bits 0-3 = |x| (0-15), bit 4 = sign x; bits 5-8 = |y|, bit 9 = sign y; bits 10-13 = |z|, bit 14 = sign z; bit 15 unused.
  - Normal = normalize(±x, ±y, ±z). Code 0 = no normal (82 of 12,931 entries).
  - Check against the normals computed from the infantry geometry: median deviation 2.0°, 99.3 % under 5° (all outliers are code 0). The best possible value with this grid would be 1.2°, so Westwood rounded slightly differently or smoothed the normals.
  - Encoding for new models: scale the normal so that the largest component is 15, round, set the sign bits.
- Feasibility of smoother infantry: PN triangles (curved patch per triangle from positions + normals, corners stay) give rounder shoulders/arms without changing proportions (Loop subdivision shrinks the figure). Level 3 = 954 triangles. Renders: `tools/experiments/models/render.py`.

## Units and movement (15.11.2026)
- `CUnitObject::Update` `0x55fe10` (vtable slot `+0x29c`): switch on the unit state `[unit+0x38]` (1–0x3a; default case logs "CUnitObject::Update, Invalid state"). Unit vtables are 0x2a8 bytes apart from `0x5d607c` to `0x5d7dc0` (plus `0x5d4f18`); all of them have `0x4413f0` at `+0x1b4`.
- Unit fields: `+0x0c` side, `+0x14/+0x18/+0x1c` position x/height/z (world units, 32 per tile, `0x4e40e0` world → tile, `0x4e41c0` tile → world), `+0x38` state, `+0x70` type (`+0` category, `+8` type name as inline chars), `+0xfc` destination vec3 (getter `0x4413f0` = vtable `+0x1b4`), `+0x140` path: up to 64 × {u8 x, u8 y}, `+0x1c0` path index, `+0x1c4` path length, `+0x1ee` path cursor.
- Move step `0x562f00` (thiscall, 1 arg, `ret 4`, starts `6A FF 68 76 C6 5C 00`): returns 0 when position x/z equals the destination exactly; otherwise follows the path, rebuilds it through `[world+0xab88]` (`0x471650`), checks/reserves tiles through `[world+0xabd8]` (`0x4ad040`). Called from many states (move, attack-move, harvest …). Hooked read-only by the jam reporter (`StuckLog.cpp`).
- Map: world `[0x817c00]`, `+0xa270` width, `+0xa274` height, `+0xa278` tiles (8 bytes; low nibble of byte 0 = object count, `+4` → int16 object indices), row start of row y = `[0x880058 + y*4]`. Object table `[0xb7d130]` (= `[world+0x104]`), 16 bytes per entry, `+4` object pointer.
- AI helper "Moving unit %s off tile as its in way" at `0x430bb6`: the AI moves its own units off a building site via `0x438e10` → `0x4c2e40` (builds a network order). The player's idle units have no such logic.

## Orders, move groups and speed (15.11.2026)
- **Command executor** `0x4c3900(packet)` (cdecl). Every order goes through it, the player's orders as well, via the network/command queue at `0x50a280`. Packet (0x138 bytes): `+8` type (0..0x32, jump table `0x4c5014`), `+0xc` side, `+0x14/+0x18/+0x1c` target x/y/z (world units), `+0x30` unit count, `+0x34` unit ids (int32, max 64), `+0x134` byte. Id → object: `0x59b350(world, &id)` (thiscall on `[0x817c00]`).
- Types 0,1,2,3,4,6,8,11,13,17 → `0x4c463a`: first `0x4abcc0(packet)` on `[world+0xabd8]` (move groups), then for each unit `unit->vtable[+0x20](packet)` (call at `0x4c47fe`–`0x4c4801`: `push ebx; mov edx,[ecx]; call [edx+0x20]`). All units get the **same** target; each unit then picks its own end tile near it. Type 0 = move (`0x4c2e40` builds it; AI "Moving unit %s off tile as its in way").
- **Move groups** `[world+0xabd8]`: up to 64 records of 0x424 bytes. Types 0,1,2,4,11,12 with ≥ 2 units get a group (`0x4abd4b`); unit `+0x200` = group index+1, `+0x204` = group serial. During path search (`0x4ad040` → `0x4ad0e0` / path search `0x471580` on `[world+0xab88]` / `0x4ad220`) the group keeps a shared usage grid of its members' planned path tiles (counts at `+0x124/+0x324`), so members spread over several paths. Destinations are not coordinated: they all aim at the same point.
- **Speed**: `rules.txt` key table at `0x61bff4` (records of 0xc8 bytes, name first; sections before it from `0x61b11c`). Parse switch `0x5267a0` (table `0x528644`, index = key - 0x2f, the handler at index k belongs to key k+1). `Speed` → type `+0xbc` (float), `HeightOffset` `+0xc4`, `RoofHeight` `+0xc8`, `TurnRate` `+0xcc`?, `CanFly` `+0x3bd`.
- **Unit::GetSpeed** `0x564d30` (vtable `+0x104`, thiscall, float in st0): type flag `+0x382` → `[unit+0x5c]`; else vtable `+0x130` (mech/deployed?) → type `+0xc0`, otherwise type `+0xbc`; minus hit slowdown (type `+0x330` × `[unit+0x22c]` / type `+0x334`). Hook point for "the whole group at the speed of the slowest unit".
- **Game frame counter:** `[[0x7cb9d4]+0x14]` (command/network manager), +1 per logic tick in `0x50a0f0`, which then executes the queued commands of that frame (packet `+0` = frame) through `0x4c3900`. About 15 ticks/s (to be confirmed in the log). Packet `+0x2c` > 0: the unit list comes from a stored group (control group).
- Blocked unit: `0x565300` sets flags in `[unit+0x1ec]` and waits a random 17–51 ticks.

## In-game messages (15.11.2026)
- `[0x8aa5c4]` message manager (0 outside a mission). `0x4e9940` manager->Add(const char* text, 0, 0) (thiscall, `ret 0xc`, starts `8B 44 24 0C 56 8B F1`) shows a line in the game's message area and copies the text (`0x4e9820`/`0x4e9bd0`). The script function `Message` (id 45, `0x4ef759`) and the hints go through `0x4e9d50`/`0x4e9d10`, which look up the string table `0xb7d098` first. One caller formats with `vsprintf` (`0x5b3148`) into a stack buffer, so the text is `char`. Used by `EmperorHooks/GameMessage.cpp`.

## Correction: 0x817c0c is a pointer (code review 15.11.2026)
`[0x817c0c]` is not a byte flag but the battlefield sidebar/HUD object (`new(0x94)` at `0x4b541a`, stored at `0x4b543f`, cleared at `0x4b7282`). Non-null = a battlefield runs. Always read the whole dword (`EmperorHooks/GameState.hpp`); the low byte of a heap pointer can be 0. Object ids: `obj+4` int16 index, `obj+6` int16 generation (checked by FindObject `0x59b350`). A new game creates a new net manager `[0x7cb9d4]` with the frame counter at 0.
