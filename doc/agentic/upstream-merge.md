# Tracking upstream KiCad

Agentic-KiCad keeps its code in its own directories, so merging upstream
`master` stays a short checklist.

## Where the fork lives

| Path | Content |
|---|---|
| `agentic/` | all fork code: protocol, transports, core, hub, viewers, and the providers compiled into each kiface |
| `qa/tests/agentic/`, `qa/tests/agentic_e2e/`, `qa/data/agentic/` | tests and fixtures |
| `doc/agentic/` | fork documentation |
| `tools/agentic/` | stage gate and generators |
| `.github/workflows/agentic-ci.yml` | fork CI |

## Hooks into upstream files

Every hook is guarded: CMake hooks by the `KICAD_AGENTIC` option, C++ hooks by
`#ifdef KICAD_AGENTIC`. Built with `-DKICAD_AGENTIC=OFF` (the default), the
tree is upstream KiCad.

The `KICAD_AGENTIC` compile definition is not global. `agentic/CMakeLists.txt`
sets it, and the fork's include root, as source properties on the hooked
files only, and on the fork's own targets. Every other object file then
compiles exactly as in upstream, so ccache shares it between `build/upstream`
and `build/agentic`. **A new C++ hook must be added to the `set_property(
SOURCE ... )` lists in `agentic/CMakeLists.txt`**, or its `#ifdef` stays
false.

| File | Hook | Stage |
|---|---|---|
| `CMakeLists.txt` | `option( KICAD_AGENTIC )`; `add_subdirectory( agentic )` after `kicad` (it adds sources to the kiface, `kicad` and `kicad-cli` targets, which must exist); `add_subdirectory( qa/tests/agentic )` after `qa` | 0 |
| `include/kiface_ids.h` | `KIFACE_AGENT_PROVIDER` as the last `KIFACE_ADDR_ID`, so files built without the definition see the same values | 0 |
| `pcbnew/pcbnew.cpp` | include of `agentic/pcb/pcb_agent_provider.h`; `case KIFACE_AGENT_PROVIDER` in `IfaceOrAddress` | 0 |
| `kicad/kicad_cli.cpp` | include of `agentic/cli/command_mcp.h`; the `mcp` command in `commandStack` | 1 |
| `kicad/kicad.cpp` | command-line switches `--classic`, `--mcp-port`, `--project`; without `--classic`, `StartAgentViewerMode()` replaces the project manager frame | 1 |
| `eeschema/eeschema.cpp` | include of `agentic/sch/sch_agent_provider.h`; `case KIFACE_AGENT_PROVIDER` in `IfaceOrAddress` (11 lines) | 3 |
| `pagelayout_editor/pl_editor.cpp` | include of `agentic/ds/ds_agent_provider.h` and `kiface_ids.h`; `IfaceOrAddress` returns `PlEditorAgentProvider()` for `KIFACE_AGENT_PROVIDER` (11 lines). The kiface is a module without an object library, so `agentic/CMakeLists.txt` adds the sources and links `agentic_ui agentic_core` to `pl_editor_kiface` itself | 12 |
| `pcb_calculator/pcb_calculator.cpp` | include of `agentic/calc/calc_agent_provider.h` and `kiface_ids.h`; `IfaceOrAddress` returns `CalculatorAgentProvider()` for `KIFACE_AGENT_PROVIDER` (11 lines). The fork's calculator sources are added to `pcb_calculator_kiface` | 13 |
| `gerbview/gerbview.cpp` | include of `agentic/gerb/gerber_agent_provider.h` and `kiface_ids.h`; `IfaceOrAddress` returns `GerbviewAgentProvider()` for `KIFACE_AGENT_PROVIDER` (13 lines) | 11 |

`agentic/CMakeLists.txt` adds the provider sources to the kiface targets
with `target_sources()`, so the kiface `CMakeLists.txt` files are not
edited. The kiface object libraries get `agentic_core` as an `INTERFACE`
link, so their own objects keep upstream's compile flags while the kiface
modules, and the upstream tests that link the object libraries, get the
fork's core.

## After a merge: calculator formulas

`agentic/calc/calc_formulas.cpp` reimplements the formulas of the calculator panels (D9).
`qa_agentic_e2e` (`test_stage13_calculator.py`) builds each upstream panel in a hidden frame
and compares its displayed results with the fork's for a table of inputs. When it fails after a
merge, upstream changed a formula: port the change to `calc_formulas.cpp`.

## Build notes

- The agentic tests are registered from the top-level `CMakeLists.txt`, not
  from `qa/`, so run them from the build root:
  `ctest --test-dir build/agentic -R '^qa_agentic'`.
- `install-deps.sh` does not install the RHI shader compilers
  (`glslang-tools`, `spirv-tools`). Install them, or unpack them without root
  into `build/hosttools` (`apt-get download glslang-tools spirv-tools` and
  `dpkg -x`); the stage gate then sets `VULKAN_SDK` to it.

## Merge procedure

1. `git fetch upstream && git merge upstream/master` on the `agentic`
   branch, at the end of a stage, never in the middle of one.
2. Resolve conflicts in the hook files listed above. Each hook is a few
   lines; keep it where the table says.
3. Run `tools/agentic/stage-gate.sh --off-build --upstream-suites "qa_common qa_eeschema qa_pcbnew"`.
4. If upstream changed a calculator formula, the calculator pinning tests
   fail (stage 13). Update `agentic/calc/` to follow.
