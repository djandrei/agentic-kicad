# Agentic KiCad

This fork of KiCad runs an MCP server inside KiCad, so an AI agent can design with
KiCad's own engine through structured tool calls: schematics, symbols, boards,
footprints, routing, 3D, simulation, Gerbers, drawing sheets, the calculator and
footprint assignment. A person can watch read-only viewers and pause the agent.

- Tool reference: [tools.md](tools.md) (generated from the registry).
- Client setup: [clients.md](clients.md).
- Upstream merges and the hooks in upstream files: [upstream-merge.md](upstream-merge.md).
- Spike results (the in-process feasibility checks): [spikes.md](spikes.md).
- Upstream test baseline: [baseline-tests.txt](baseline-tests.txt); timings: [timings.json](timings.json).

## Two hosts

| Host | Command | Viewers |
|---|---|---|
| Headless | `kicad-cli mcp --stdio`, or `kicad-cli mcp --port 0` for HTTP on 127.0.0.1 | none |
| Viewer mode | `kicad` (agent mode is the default; `kicad --classic` starts upstream's GUI) with `--mcp-port N` | the hub and one viewer per program |

Both run the same `AGENT_HOST` (`agentic/core/agent_host.*`): an MCP server
(`agentic/protocol/`), a transport (`agentic/transport/`: stdio, or HTTP with a
bearer token printed on stderr), the tool registry, resources and prompts.

## Concepts

- **Programs.** Each KiCad program is an `AGENT_PROVIDER` compiled into its kiface and
  returned by `IfaceOrAddress( KIFACE_AGENT_PROVIDER )`: schematic (`sch`) with the
  symbol editor (`sym`) and simulator (`sim`) in eeschema; PCB (`pcb`) with the
  footprint editor (`fp`) and 3D (`view3d`) in pcbnew; `gerber`, `ds` (drawing sheets),
  `calc` and `cvpcb` in their kifaces. A provider registers its tools (one tool group
  per program) and creates its viewer.
- **Projects and sessions.** `SESSION_MANAGER` holds open projects (`AGENT_PROJECT`).
  A project owns document sessions (`DOC_SESSION`: a schematic, a board, a symbol or
  footprint library, a drawing sheet), each holding its document in memory with a lock
  and the content versions of its files. Tools act on the current project or on the one
  named by their `project` argument.
- **Every change is saved** (D5). A changing call runs inside `AGENT_PROJECT::Mutate()`:
  the change goes through the program's commit (`SCH_COMMIT`, `BOARD_COMMIT`) where there
  is one, the touched files are checked for outside changes and saved atomically, and the
  call becomes one step of the project's file history. `undo` and `redo` restore saved
  versions and reload the documents. `batch` runs several calls with one save and one undo
  step.
- **Sandbox.** Paths are project-relative and cannot leave the project folder
  (`FILE_STORE`). Libraries outside the project are read, never changed.
- **Viewers** (D6) open the first time the agent uses a program. They follow commits,
  reloads and results through `SESSION_OBSERVER` events; they never change documents.
  The person can pause the agent from the hub.
- **Long calls** report progress and honour cancellation and `timeoutSeconds` (D10).
- **Checks agree with upstream.** ERC, DRC, Gerber information and exports are KiCad's
  own code; tests compare them with `kicad-cli` on the same files. The calculator's
  formulas are reimplemented (D9) and pinned to the upstream panels by a test.

## Source layout

| Directory | Contents |
|---|---|
| `agentic/protocol`, `agentic/transport` | JSON-RPC, MCP server, schema validation; stdio and HTTP |
| `agentic/core` | host, sessions, projects, file store and history, libraries, PCM, docs, core tools |
| `agentic/hub`, `agentic/viewer_shell` | viewer mode: the hub frame and the viewer base class |
| `agentic/sch`, `agentic/pcb`, `agentic/gerb`, `agentic/ds`, `agentic/calc`, `agentic/cvpcb` | the programs |
| `agentic/cli` | `kicad-cli mcp` |
| `qa/tests/agentic`, `qa/tests/agentic_e2e`, `qa/tests/agentic_perf` | Boost tests, end-to-end tests, timings |
| `tools/agentic` | stage gate, baseline comparison, tool reference generator |

Fork code is isolated in these directories; upstream files are touched only by the small
`#ifdef KICAD_AGENTIC` hooks listed in [upstream-merge.md](upstream-merge.md), and
`KICAD_AGENTIC` is defined only on those files, so every other object is built exactly as
upstream builds it.

## Verifying a change

```bash
xvfb-run -a tools/agentic/stage-gate.sh                       # build, agentic tests, timings, tool reference, format
xvfb-run -a tools/agentic/stage-gate.sh --off-build \
    --upstream-suites "qa_common qa_eeschema qa_pcbnew"       # also the OFF build and upstream suites
```
