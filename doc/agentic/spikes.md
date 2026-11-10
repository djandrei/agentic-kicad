# Stage 0.2 spikes

The spikes are throwaway programs under `qa/tools/agentic_spikes/` (not
committed). Each has a pass criterion. A failed spike changes the design
before the stage that depends on it.

Machine: Intel Core i7-10700K (16 threads), ext4 on local SSD, Ubuntu 24.04,
`build/agentic` (QABUILD, KICAD_STDLIB_LIGHT_DEBUG). Display spikes ran
under `xvfb-run` (1600x1200x24, Mesa).

| Spike | Result |
|---|---|
| S0.a Frameless schematic | **pass** |
| S0.b Frameless board | **pass** (identical to `kicad-cli pcb drc`, two boards) |
| S0.c Save and reload cost | **done**: measured, see below |
| S0.d Headless PNS | **pass** (DRC unchanged by the route) |
| S0.e Canvas on a session | **pass** (board and schematic, OpenGL and Cairo) |
| S0.f Offscreen pictures | **pass** (no `DISPLAY`) |
| S0.g Simulator in-process | **pass** for the in-process path; XSPICE models need the `ngspice` package |

## S0.a Frameless schematic

Program `spike_frameless_sch`, run on a copy of `demos/ecc83`. On a frameless
`TOOL_MANAGER` it takes `ecc83-pp:R` from the sheet's embedded symbol cache,
places R900 to the right of the drawing, draws a wire from pin 1 and puts the
local label `AGENTIC_NET` on its end, in one `SCH_COMMIT`, then saves the root
sheet with `SCH_API_SAVE::SaveSheetToFile`.

- The commit recomputed connectivity by itself (2 ms): the pin is on
  `/AGENTIC_NET` in memory.
- `kicad-cli sch export netlist` on the saved file lists `/AGENTIC_NET` with
  R900 pin 1 (13 nets before, 15 after: the new net and R900's unconnected
  pin 2).
- `kicad-cli sch erc`: the 6 violations of the original plus exactly the two
  the edit should cause (`isolated_pin_label`, `pin_not_connected`).

Findings:

- **Connectivity is read through `SCHEMATIC::Connectivity()`.** Upstream's
  default engine is the `SCH_CONNECTIVITY::FACADE`
  (`ADVANCED_CFG::m_ConnectivityEngine = true`). `SCH_ITEM::Connection()` and
  `CONNECTION_GRAPH` return nothing for new items. Net queries in the
  schematic tools use `Connectivity().Connection( item uuid, sheet path )`,
  `NetByName()` and `NetNames()`. Net names carry the sheet prefix
  (`/AGENTIC_NET`).
- **A frameless `SCH_COMMIT::Push` recomputes connectivity** (it calls
  `SCHEMATIC::RecalculateConnections` when there is no frame), so a
  schematic operation needs no explicit refresh.
- New items need their parent screen before `commit.Add` (as found in S0.e).

## S0.b Frameless board

Program `spike_frameless_pcb`, the steps `kicad-cli pcb drc` takes, on a
frameless `TOOL_MANAGER`: add a 2 mm track from the first pad that has a
net, through `BOARD_COMMIT`; load the footprint libraries; refill every zone
with `ZONE_FILLER_TOOL::FillAllZones( headless )`; run `DRC_ENGINE` with the
drawing-sheet proxy, committing markers; write the JSON report; save. Then
`kicad-cli pcb drc --format json` runs on the saved file and the reports are
compared item for item (type, severity, item uuids and positions).

| Board | Zones | Fill | DRC | Save | Spike | `kicad-cli` | Identical |
|---|---|---|---|---|---|---|---|
| `pic_programmer` | 1 | 374 ms | 175 ms | 35 ms | 59 violations (58 `lib_footprint_issues`, 1 `track_dangling`), 0 unconnected | same | yes |
| `video` | 2 | 2303 ms | 817 ms | 312 ms | 3 violations (1 `shorting_items`, 2 `solder_mask_bridge`), 0 unconnected | same | yes |

Findings:

- **Zone filling goes through `ZONE_FILLER_TOOL`, not bare `ZONE_FILLER`.**
  The tool, headless, also updates teardrops and regenerates dirty
  generators (via stitching) before and after the fill, as the editor and
  `kicad-cli` do. It registers on a frameless `TOOL_MANAGER`.
- DRC's library checks need the footprint libraries loaded first
  (`PROJECT_PCB::FootprintLibAdapter( project )->AsyncLoad()` and
  `BlockUntilLoaded()`).
- `PCBNEW_JOBS_HANDLER::JobExportDrc` is the reference sequence; the
  board DRC tool can reuse it nearly line for line.

## S0.c Save and reload cost

Programs `spike_save_pcb` and `spike_save_sch`, run on copies of the demo
projects. Each repetition does what a mutating call and an undo do:

- history: copy the current file into `.agentic/history/` (D7);
- save: save in place through the upstream writer (D5);
- reload: destroy the in-memory document and load it from disk (undo, D7).

Medians of 3 repetitions, in milliseconds. No wx asserts were raised.

| Board | Size | Footprints | Tracks and vias | Zones | First load | History copy | Save | Reload | Peak RSS |
|---|---|---|---|---|---|---|---|---|---|
| `video` | 5.9 MB | 189 | 8740 | 2 | 475 | 4 | 298 | 412 | 345 MB |
| `vme-wren` | 70.6 MB | 1508 | 31024 | 128 | 3645 | 51 | 2639 | 3735 | 1148 MB |
| `jetson-agx-thor-baseboard` | 85.7 MB | 1125 | 25386 | 125 | 3751 | 63 | 3727 | 3737 | 1154 MB |

| Schematic | Sheet files | Sheet instances | Size | First load | Save, all sheets | Save, slowest sheet | Reload | Peak RSS |
|---|---|---|---|---|---|---|---|---|
| `video` | 8 | 8 | 1.9 MB | 398 | 93 | 18 | 364 | 260 MB |
| `jetson-agx-thor-baseboard` | 15 | 15 | 11.0 MB | 2078 | 431 | 55 | 1777 | 624 MB |
| `vme-wren` | 36 | 73 | 19.6 MB | 2808 | 879 | 54 | 2549 | 644 MB |

Findings:

- **Saves are already atomic upstream.** `PRETTIFIED_FILE_OUTPUTFORMATTER`
  (used by the board and schematic writers) writes a unique sibling temp
  file, fsyncs it, renames it over the target and fsyncs the directory; a
  failure leaves the target untouched (`common/richio.cpp`,
  `SIBLING_TEMP_FILE`). D5 needs no temp-and-rename of its own; the save
  times above include the fsync.
- **Typical designs:** saving after every call costs about 0.3 s for a board
  and under 0.1 s for a schematic sheet. D5 holds as designed.
- **The largest boards:** 2.6 to 3.7 s per mutating call, and about 3.7 s
  per undo, with about 1.15 GB of memory per open board. This is the case
  DESIGN.md's risk section foresaw. The remedy stays a batch call that
  applies several operations and saves once; it is needed from the first
  PCB stage (6), not as a later optimisation.
- **Schematic undo** reloads the whole hierarchy (2.5 s on `vme-wren`) even
  when one sheet changed, because sheet files can be shared between
  instances. Saving a single sheet is cheap (under 60 ms). Reloading only the
  touched sheets is possible but not needed for version 1.
- **History copies** cost under 70 ms even for the largest board.

## S0.d Headless PNS

Program `spike_pns`, run on a copy of `demos/pic_programmer`. It picks a
routed net with exactly two top-side pads 5 to 40 mm apart
(`Net-(U4-FB+)`, U4.2 to RV1.2, 29.4 mm), removes its 5 tracks through
`BOARD_COMMIT` (1 unconnected item), then routes pad to pad with
`PNS::ROUTER` in walkaround mode. Track width comes from the net class
(`ImportSizes`, 0.5 mm).

`HEADLESS_PNS_IFACE` subclasses `PNS_KICAD_IFACE_BASE`. Its `AddItem` and
`RemoveItem` turn PNS segments, arcs and vias into board items in one
`BOARD_COMMIT`, which its `Commit` pushes. Upstream's `PNS_KICAD_IFACE`
cannot be used: it needs a `PCB_TOOL_BASE` and a `VIEW`.

- The head reached the end pad; `FixRoute` finished the route; 3 segments
  were committed in 115 ms. The net has no unconnected edge left.
- DRC (`kicad-cli`) on the routed board equals DRC on a copy that was only
  loaded and saved, item for item (58 `lib_footprint_issues`, 0 unconnected).

Findings:

- **The first save upgrades the file format, and that can change DRC.**
  The demo is version 20260206; a load and save writes 20261031, and DRC on
  the upgraded copy reports 12 fewer violations than on the original
  (`hole_clearance`, `silk_edge_clearance`, `starved_thermal`). Tests that
  compare DRC before and after an edit use a re-saved copy as the baseline.
  The agent should be told when a call upgraded a file (Stage 2 records the
  file version in the call result).
- The router's own `PNS_KICAD_IFACE::createBoardItem` is protected and not
  virtual. The headless interface duplicates it (about 50 lines: segment,
  arc, via). Footprint drag (`SOLID_T` offsets) and group membership
  are not needed for routing and stay out.

## S0.e Canvas on a session

Programs `spike_canvas_pcb` and `spike_canvas_sch`, run on the `video` demo.
A "session" owns the document and a frameless `TOOL_MANAGER` (as
`HEADLESS_PCB_CONTEXT` and `HEADLESS_SCH_CONTEXT` do). A plain `wxFrame`
holds `PCB_DRAW_PANEL_GAL` or `SCH_DRAW_PANEL` drawing that document. The
canvas only observes: a `BOARD_LISTENER` or `SCHEMATIC_LISTENER` adds,
removes and updates view items when the session commits. The session then
commits a filled rectangle through `BOARD_COMMIT` or `SCH_COMMIT`. The
screen is captured before and after (`xwd`, cropped to the canvas).

Pass criterion: the canvas draws the document (more than 8 colours), the
listener is called by the commit, and more than half of the pixels inside
the rectangle change.

| Canvas | GAL | Listener calls | Commit | Pixels changed in the rectangle | Result |
|---|---|---|---|---|---|
| board | OpenGL | 1 | 112 ms | 100 % | pass |
| board | Cairo | 1 | 172 ms | 100 % | pass |
| schematic | OpenGL | 1 | 38 ms | 98 % | pass |
| schematic | Cairo | 1 | 32 ms | 97 % | pass |

Findings for the viewer design (stage 2 and the program stages):

- **Listeners are the update path.** A frameless `BOARD_COMMIT::Push` calls
  `BOARD::OnItemsCompositeUpdate`, and a frameless `SCH_COMMIT::Push` calls
  `SCHEMATIC::OnItemsAdded`/`Removed`/`Changed`. A viewer subscribes to its
  document and mirrors the changes into its own view. No upstream hook is
  needed, and several viewers can follow one document.
- **A new schematic item needs its parent before the commit.**
  `SCH_COMMIT::Push` finds the schematic to notify through the first item's
  parents, before it appends the item to the screen. A new item without a
  parent is added, but no listener is called. Schematic operations must set
  the item's parent screen before `commit.Add`.
- **A view must be cleared before its document goes away.** Board and
  schematic items unregister themselves from their view when destroyed. If
  the canvas is destroyed first, or the document is deleted while a canvas
  still shows it, that is a use after free. The session must tell viewers
  to `Clear()` before it deletes or reloads a document (undo, reload,
  close), not after.
- **The frame type sets the painter's settings.** A canvas whose parent is
  not a KiCad frame paints as `FRAME_FOOTPRINT_PREVIEW` and reads the CvPcb
  display settings (`pcb_painter.cpp`). The schematic painter reads
  `Kiface().KifaceSettings()`. Viewers compiled into their kiface get the
  kiface's settings; the PCB viewer frame should report the PCB editor's
  frame type, or set the painter's display options itself.
- **Reading an OpenGL canvas back through a `wxClientDC` is unreliable.**
  It returned the first frame only, then stale pixels. Captures for the
  agent stay offscreen (plotters and the raytracer, as designed); viewer
  tests capture the X screen.

## S0.f Offscreen pictures

Upstream `kicad-cli` already plots PNG through `PNG_PLOTTER`
(`sch export png`, `pcb export png`) and renders 3D through
`RENDER_3D_RAYTRACE_RAM` (`pcb render`). All three ran on a copy of
`demos/video` with `DISPLAY` and `WAYLAND_DISPLAY` unset:

| Command | Output | Time |
|---|---|---|
| `sch export png` | 8 sheets, 3508x2480, 4580 colours on the root sheet | 8.6 s |
| `pcb export png -l F.Cu,B.Cu,F.SilkS,Edge.Cuts` | 4 layers, 4961x3508 | 4.1 s |
| `pcb render` (basic quality) | 1568x872 (1600x900 requested), the board seen from the top | 2.6 s (0.26 s raytracing) |

Findings:

- The picture tools call the same job handlers in-process
  (`EESCHEMA_JOBS_HANDLER`, `PCBNEW_JOBS_HANDLER::JobExportRender`); nothing
  needs a display.
- The default resolutions are larger than an agent needs; the tools take a
  width and crop or scale to it.
- The raytracer rounds the image size down to its block size; the tool
  reports the actual size.

## S0.g Simulator in-process

Program `spike_sim`. It loads the schematic, builds `SPICE_CIRCUIT_MODEL`
(the simulator frame's model), reads the analysis from the sheet's text
directives, attaches it to `SPICE_SIMULATOR::CreateInstance( "ngspice" )`,
runs it on ngspice's thread, waits, and reads the vectors.

| Demo | Analysis | Attach | Run | Result |
|---|---|---|---|---|
| `simulation/rectifier` | `.tran 1u 10m` | 2.4 ms | 100 ms | pass: 14 vectors, 10008 points; `/signal_in` +-1.0 V, `/rect_out` 0 to 0.399 V |
| `simulation/sallen_key` | `.ac dec 10 1 1Meg` | 4.3 ms | 20 ms | fails in ngspice: `MIF-ERROR - unable to find definition of model a$poly$e.xu1.eos` |

Findings:

- **The in-process path works without the frame.** The steps the frame takes
  have to be repeated by the session:
  - call `ReadSchematicAndLibraries` before `GetSchTextSimCommand`, or the
    model has no sheets and returns an empty command;
  - assign the project's `NGSPICE_SETTINGS` to the simulator before `Init()`,
    with the LTspice/PSpice compatibility mode when the project has no
    workbook. Otherwise `NGSPICE::GetSettingCommands` asserts.
  - AC vectors are complex: `GetRealVector` returns nothing for them; use
    `GetGainVector` and `GetPhaseVector`.
- **XSPICE code models are missing on this machine.** `sallen_key`'s op-amp
  model uses `spice2poly`, an XSPICE code model in the `ngspice` package
  (`spice2poly.cm`). Only `libngspice0` is installed, and `install-deps.sh`
  lists only `libngspice0-dev`. The `qa_spice` baseline failures may have
  the same cause. Installing `ngspice` is the owner's decision; until then
  the simulator stage tests with models that need no code models, and its
  tool reports `MIF-ERROR` as a missing-dependency error.
- ngspice writes its console from its own thread. The session captures it
  through `SetReporter` and detaches the reporter before reading it
  (`SetReporter( nullptr )` waits for an in-flight callback).
