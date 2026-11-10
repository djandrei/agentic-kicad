# Connecting an MCP client

## Stdio (the client starts KiCad)

The client spawns `kicad-cli mcp --stdio` and talks JSON-RPC over its stdin and stdout;
stderr carries the log. Optionally `--project DIR` opens a project at start.

Claude Code:

```bash
claude mcp add kicad -- /path/to/kicad-cli mcp --stdio
```

A JSON configuration (Claude Desktop and most other clients):

```json
{
  "mcpServers": {
    "kicad": { "command": "/path/to/kicad-cli", "args": [ "mcp", "--stdio" ] }
  }
}
```

When KiCad is not installed, set `LD_LIBRARY_PATH` (or use `setup-env.sh`) so `kicad-cli`
finds its libraries, and point the versioned library variables (`KICAD<version>_SYMBOL_DIR`,
`KICAD<version>_FOOTPRINT_DIR`, ...) at the
libraries you want the agent to use.

## HTTP (streamable HTTP on 127.0.0.1)

Headless: `kicad-cli mcp --port 0`. With viewers: `kicad --mcp-port 0` (or a fixed port).
The server prints on stderr:

```
... listening at http://127.0.0.1:PORT/mcp
... Bearer token: TOKEN
```

Configure the client with that URL and an `Authorization: Bearer TOKEN` header, e.g.

```bash
claude mcp add --transport http kicad http://127.0.0.1:PORT/mcp --header "Authorization: Bearer TOKEN"
```

The server only listens on the loopback interface, requires the token on every request,
and refuses browser origins other than localhost.

## First calls

1. `status` - the mode, the open projects and the programs available (and ngspice).
2. `project_create { path }` or `project_open { path }`.
3. `docs_search` / the prompts give the working loops for each program.
4. `project_set { disabledGroups: [...] }` hides tool groups a task does not need; clients
   that support `notifications/tools/list_changed` see the shorter list at once.

The tools are listed in [tools.md](tools.md).
