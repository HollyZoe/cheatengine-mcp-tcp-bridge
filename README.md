**[English](README.md) | [中文](README_CN.md)**

# Cheat Engine MCP Bridge — TCP Enhanced Edition

[![Version](https://img.shields.io/badge/version-15.0.0-blue.svg)](#) [![Python](https://img.shields.io/badge/python-3.10%2B-green.svg)](https://python.org) [![Transport](https://img.shields.io/badge/transport-Native%20TCP%20DLL-orange.svg)](#)

> A high-performance fork of [miscusi-peek/cheatengine-mcp-bridge](https://github.com/miscusi-peek/cheatengine-mcp-bridge) that replaces the Windows Named Pipe transport with a native C TCP bridge, enabling **remote Cheat Engine control**, **zero pywin32 dependency**, and **multi-instance support** out of the box.

[Demo Video](https://github.com/user-attachments/assets/a184a006-f569-4b55-858a-ed80a7139035)

---

## What Changed from the Original

This fork diverges significantly from the original v12.0.0 project. Below is a side-by-side comparison of every major difference.

### Architecture Comparison

```
ORIGINAL (v12.0.0)                         THIS FORK (v15.0.0)
─────────────────                          ──────────────────
AI Client                                  AI Client
  │ stdio JSON-RPC                           │ stdio JSON-RPC
  ▼                                          ▼
mcp_cheatengine.py                         mcp_cheatengine.py
  │ Named Pipe (pywin32)                     │ TCP socket (stdlib only)
  ▼                                          ▼
\\.\pipe\CE_MCP_Bridge_v99                 ce_mcp_tcp.dll (native C)
  │                                          │ Winsock2 + select()
  ▼                                          ▼
ce_mcp_bridge.lua                          ce_mcp_bridge.lua
  │ Worker thread (Lua pipe I/O)             │ 1ms timer poll loop
  ▼                                          ▼
Target Process                             Target Process
```

### Feature Comparison

| Feature | Original (v12.0.0) | This Fork (v15.0.0) |
|---------|:-------------------:|:--------------------:|
| **Transport** | Windows Named Pipe | Native TCP (compiled C DLL) |
| **Remote CE support** | Via separate `ce_tcp_relay.py` script | Built-in (`CE_HOST` env var) |
| **Python dependencies** | `mcp` + `pywin32` | `mcp` only |
| **Multi-instance** | Not supported | Port auto-increment (17171–17181) |
| **Default timeout** | 30 seconds | 90 seconds |
| **CE Lua I/O model** | Worker thread with blocking pipe | 1ms timer poll via DLL |
| **Native code** | None | `ce_mcp_tcp_x64.dll` / `ce_mcp_tcp_x86.dll` |
| **DLL debug console** | N/A | Separate diagnostic console window |
| **Setup complexity** | `pip install` only | `pip install` + copy DLL to CE directory |
| **Cross-platform MCP server** | Requires relay script | Works natively (TCP is stdlib) |
| **Lua codebase** | ~6700 lines (includes FFI/Winsock/Pipe code) | ~5700 lines (1000 lines of dead code removed) |
| **CRT dependency** | N/A | Static `/MT` link — no VC runtime needed |
| **Chinese documentation** | No | Yes (`README_CN.md`) |

### What We Gained

- **True remote debugging** — Point `CE_HOST` at any machine running CE. No relay scripts, no extra processes.
- **No pywin32** — The default TCP transport uses only Python stdlib. Simpler install, fewer failure points.
- **Multi-CE-instance support** — Run multiple CE instances simultaneously; each gets its own port (17171, 17172, ...). The Python client auto-discovers the right one via `ping` verification.
- **Native C performance** — TCP I/O is handled by a compiled DLL, not Lua FFI. No `getAddressSafe` crashes, no PEB-walk failures, no kernel32 FFI issues.
- **Higher reliability** — 90s default timeout (vs 30s), 3x retry with auto-reconnect, thread-safe lock.
- **DLL debug console** — A dedicated console window shows TCP state, Lua API resolution, and connection events in real time. Makes troubleshooting trivial.
- **Cleaner Lua script** — Removed ~1000 lines of dead Winsock FFI, pipe worker threads, and kernel32 bootstrap code.

### What We Lost

- **Zero-DLL simplicity** — The original needed only Python packages. This fork requires placing a DLL next to the CE executable (one extra step).
- **Built-in TCP relay** — The original included `ce_tcp_relay.py` for bridging Named Pipe to TCP. This fork doesn't need it (TCP is native), but the relay script itself was removed.
- **Pipe transport** — Still available via `CE_TRANSPORT=pipe` + `pywin32`, but deprecated and no longer the default.

### Security Note

The TCP bridge has **no authentication or encryption**. The original's Named Pipe was local-only by design, which provided implicit access control. With TCP, the port is reachable over the network. **Only expose on trusted networks (VPN, LAN). Never open port 17171 to the public internet.**

---

## What This Project Does

Connect AI agents (Claude, Cursor, Copilot, Codex) to Cheat Engine via the Model Context Protocol. The AI can then read memory, scan for values, set hardware breakpoints, disassemble functions, inject code, and perform ~180 other operations on any attached process — all through natural language.

```
You: "Scan for gold: 15000"        →  AI finds 47 results
You: "Gold changed to 15100"       →  AI filters to 3 addresses
You: "What writes to the first?"   →  AI sets hardware breakpoint
You: "Disassemble that function"   →  Full AddGold logic revealed
```

---

## Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| Python | 3.10+ | MCP server runtime |
| Cheat Engine | 7.5+ | 7.6 recommended; DBVM features need DBVM-enabled build |
| Native TCP DLL | v2.0.0 | `ce_mcp_tcp_x64.dll` or `_x86.dll` — must match CE architecture |
| pip package `mcp` | latest | `pip install mcp` |

> `pywin32` is only needed if you use the legacy Named Pipe mode (`CE_TRANSPORT=pipe`). TCP mode (default) has no extra dependencies.

---

## Setup

### 1. Clone & Install

```bash
git clone https://github.com/HollyZoe/cheatengine-mcp-tcp-bridge.git
cd cheatengine-mcp-tcp-bridge
pip install -r MCP_Server/requirements.txt
```

### 2. Place the DLL

Copy the architecture-matching DLL into your **Cheat Engine executable directory**:

| CE Build | DLL to Copy |
|----------|-------------|
| 64-bit | `ce_mcp_tcp_x64.dll` |
| 32-bit | `ce_mcp_tcp_x86.dll` |

Source locations: `MCP_Server/` or `NativeBridge/bin/x64/` and `NativeBridge/bin/x86/`.

```
C:\CE 7.5\cheatengine-x86_64.exe
C:\CE 7.5\ce_mcp_tcp_x64.dll    ← place here
```

### 3. Load in Cheat Engine

1. Attach CE to your target process.
2. Load the bridge script:
   - **Recommended:** `File` → `Execute Script` → open `MCP_Server/ce_mcp_bridge.lua` → `Execute`
   - **Alternative:** `Table` → `Show Cheat Table Lua Script` → paste and execute:
     ```lua
     dofile([[C:\path\to\cheatengine-mcp-tcp-bridge\MCP_Server\ce_mcp_bridge.lua]])
     ```

Expected output:
```
[MCP] CE x64 - loading ce_mcp_tcp_x64.dll
[MCP] DLL loaded OK from: C:\CE 7.5\ce_mcp_tcp_x64.dll
[MCP] Bridge started on port 17171 (native TCP, 1ms poll)
```

### 4. Configure Your AI Client

<details>
<summary><b>Cursor IDE</b></summary>

`.cursor/mcp.json`:
```json
{
  "mcpServers": {
    "cheatengine": {
      "command": "python",
      "args": ["C:/path/to/cheatengine-mcp-tcp-bridge/MCP_Server/mcp_cheatengine.py"],
      "env": {
        "CE_TRANSPORT": "tcp",
        "CE_HOST": "127.0.0.1",
        "CE_PORT": "17171"
      }
    }
  }
}
```
Restart Cursor after saving.
</details>

<details>
<summary><b>Claude Desktop</b></summary>

`%APPDATA%\Claude\claude_desktop_config.json`:
```json
{
  "mcpServers": {
    "cheatengine": {
      "command": "python",
      "args": ["C:/path/to/cheatengine-mcp-tcp-bridge/MCP_Server/mcp_cheatengine.py"],
      "env": {
        "CE_TRANSPORT": "tcp",
        "CE_HOST": "127.0.0.1",
        "CE_PORT": "17171"
      }
    }
  }
}
```
</details>

<details>
<summary><b>Codex CLI</b></summary>

`~/.codex/config.toml`:
```toml
[mcp_servers.cheatengine]
command = "python"
args = ['C:\path\to\cheatengine-mcp-tcp-bridge\MCP_Server\mcp_cheatengine.py']
```
</details>

<details>
<summary><b>Remote Cheat Engine</b></summary>

Set `CE_HOST` to the remote machine's IP:
```json
{
  "env": {
    "CE_HOST": "192.168.1.100",
    "CE_PORT": "17171"
  }
}
```

Firewall on the CE machine:
```powershell
netsh advfirewall firewall add rule name="CE MCP Bridge" dir=in action=allow protocol=TCP localport=17171
```
</details>

### 5. Verify

Ask the AI: *"Ping the Cheat Engine bridge"*

```json
{"success": true, "version": "15.0.0", "message": "CE MCP Bridge v15.0.0 alive"}
```

---

## Available Tools (~180)

### Memory Operations
| Tool | Description |
|------|-------------|
| `read_memory`, `read_integer`, `read_string` | Read any data type from process memory |
| `write_memory`, `write_integer`, `write_string` | Write values to process memory |
| `read_pointer_chain` | Follow multi-level pointer paths like `[[base+0x10]+0x20]` |
| `scan_all`, `next_scan`, `aob_scan` | Value scanning and AOB pattern matching |
| `allocate_memory`, `free_memory` | Allocate/free memory in the target process |

### Code Analysis
| Tool | Description |
|------|-------------|
| `disassemble` | Disassemble instructions at an address |
| `analyze_function` | Find all CALL instructions within a function |
| `find_function_boundaries` | Detect function start/end addresses |
| `dissect_structure` | Auto-detect structure fields and types |
| `get_rtti_classname` | Identify C++ object types via RTTI |
| `find_references` | Find instructions that reference an address |

### Debugging
| Tool | Description |
|------|-------------|
| `set_breakpoint`, `set_data_breakpoint` | Hardware breakpoints (DR0–DR3) |
| `start_dbvm_watch` | Ring -1 invisible tracing via DBVM |
| `get_breakpoint_hits` | Retrieve captured register/stack data |

### Code Injection
| Tool | Description |
|------|-------------|
| `auto_assemble` | Execute CE auto-assembler scripts |
| `inject_dll` | Load a DLL into the target process |
| `execute_code`, `execute_method` | Run shellcode or call methods remotely |
| `compile_c_code` | Compile C source into injectable shellcode |

### Process & Modules
| Tool | Description |
|------|-------------|
| `get_process_info`, `get_process_list` | Process enumeration and info |
| `open_process`, `create_process` | Attach to or launch processes |
| `enum_modules`, `get_symbol_address` | Module and symbol resolution |

### Kernel Mode (DBK / DBVM)
| Tool | Description |
|------|-------------|
| `dbk_get_cr3`, `dbk_get_cr0` | Read control registers |
| `read_process_memory_cr3` | Physical memory read via CR3 bypass |
| `get_physical_address` | Virtual to physical address translation |

### Other Categories
- **Symbol Management**: `register_symbol`, `get_symbol_info`, `enable_windows_symbols`
- **Assembly**: `assemble_instruction`, `generate_api_hook_script`
- **GUI Automation**: `find_window`, `send_window_message`
- **Input**: `get_pixel`, `is_key_pressed`, `do_key_press`, `set_mouse_pos`
- **Cheat Table**: `load_table`, `save_table`, `get_address_list`, `create_memory_record`
- **File/System**: `file_exists`, `run_command`, `shell_execute`
- **Structure**: `create_structure`, `add_element_to_structure`, `export_structure_to_xml`

Full reference: `AI_Context/MCP_Bridge_Command_Reference.md`

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CE_TRANSPORT` | `tcp` | `tcp` (recommended) or `pipe` (legacy, requires pywin32) |
| `CE_HOST` | `127.0.0.1` | CE instance IP. Set to remote IP for network debugging |
| `CE_PORT` | `17171` | Base TCP port. DLL auto-increments if occupied |
| `CE_PORT_RANGE` | `10` | Number of ports to scan from `CE_PORT` |
| `CE_MCP_TIMEOUT` | `90` | Per-tool timeout in seconds. `0` = no timeout |
| `CE_MCP_ALLOW_SHELL` | *(unset)* | Set `1` to enable `run_command`/`shell_execute`. Security risk |

---

## Technical Architecture

### Native TCP DLL (`ce_mcp_tcp.c`)

The DLL (compiled C, static CRT) is loaded by CE Lua via `package.loadlib`. It:

1. Dynamically resolves 17 Lua C API functions from CE's `lua53-64.dll`
2. Registers 5 global Lua functions: `mcp_tcp_start/stop/poll/respond/status`
3. Runs a dedicated TCP server thread (Winsock2 + `select()`)
4. Binds to `0.0.0.0:17171`, auto-increments port if busy
5. Queues incoming JSON-RPC requests for Lua to dequeue via `mcp_tcp_poll()`
6. Sends responses back via `mcp_tcp_respond()` with 4-byte LE length-prefix framing

### Wire Protocol

Both TCP and legacy Pipe use the same framing:

```
[4 bytes: payload length, little-endian uint32] [N bytes: UTF-8 JSON-RPC]
```

### Python Client Behavior

- Auto-scans ports 17171–17180, verifying each with `ping`
- 3 retries with auto-reconnect on failure
- Thread-safe via `threading.Lock()`
- Configurable timeout with automatic socket cleanup

---

## Critical Configuration

> **BSOD Prevention**: You **must** disable Cheat Engine → Settings → Extra → **"Query memory region routines"**. With it enabled, memory scans on DBVM-protected pages trigger `CLOCK_WATCHDOG_TIMEOUT` BSODs.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| CE says "too many local variables" | Use `dofile(...)` from disk instead of pasting the script |
| DLL not found | Copy `ce_mcp_tcp_x64.dll` (or `_x86.dll`) to the CE executable directory |
| Lua API resolution failed | Check the DLL debug console for module detection. Ensure CE 7.5+ with Lua 5.3 |
| Cannot connect (TCP) | Verify `netstat -an | findstr 17171`, check CE_HOST/CE_PORT, restart IDE |
| Cannot connect (Pipe/legacy) | `pip install pywin32`, set `CE_TRANSPORT=pipe`, verify pipe name in CE output |
| Timeout on heavy operations | Increase `CE_MCP_TIMEOUT` (e.g., `180`) |
| CE UI freezes during commands | Expected — handlers run on CE main thread for API safety |

---

## Project Structure

```
README.md / README_CN.md          Documentation (EN/CN)
CLAUDE.md / AGENTS.md             AI agent coding guidance

MCP_Server/
├── mcp_cheatengine.py            Python MCP server (~2350 lines)
├── ce_mcp_bridge.lua             CE Lua bridge (~5700 lines)
├── ce_mcp_tcp_x64.dll            Pre-built 64-bit native TCP DLL
├── ce_mcp_tcp_x86.dll            Pre-built 32-bit native TCP DLL
├── test_mcp.py                   Integration test suite
└── requirements.txt              Python dependencies

NativeBridge/
├── ce_mcp_tcp.c                  DLL source code (~770 lines)
├── build.bat                     Build script (VS Build Tools)
└── bin/{x64,x86}/                Pre-built binaries

AI_Context/
├── MCP_Bridge_Command_Reference.md
├── AI_Guide_MCP_Server_Implementation.md
├── CE_LUA_Documentation.md
└── BATCH_WORKER_BRIEFING.md
```

---

## Tool Testing Results (v15.0.0)

Full interface testing was performed on all 161 registered MCP tools. Results summary:

| Category | Result | Count |
|----------|--------|-------|
| **Passed** | All operations returned expected results | **110+** |
| **Fixed during testing** | Bugs found and patched | **3** |
| **CE environment limitation** | Functions not available in current CE build | **5** |
| **Requires kernel driver** | Skipped (needs signed driver / DBVM) | **~20** |
| **Dangerous ops (need correct params)** | Work correctly but crash CE with invalid addresses | **3** |

### Bugs Fixed During Testing

| Tool | Issue | Fix |
|------|-------|-----|
| `get_memory_protection` | Called non-existent `getMemoryProtection()` CE function | Reimplemented using `enumMemoryRegions()` to find the containing region |
| `get_memory_regions` | Same root cause — returned 0 regions | Reimplemented using `enumMemoryRegions()`, now returns all committed regions with pagination |
| `debug_get_current_debugger_interface` | Called `debug_getCurrentDebuggerInterface()` without safety checks, crashing CE | Added nil-check and `debug_isDebugging()` guard before calling |

### CE Environment Limitations (Not Bugs)

| Tool | Reason |
|------|--------|
| `compile_c_code` | TCC compiler library (`tcc64-64.dll`) not installed |
| `compile_cs_code` | .NET compiler not available (`compileCS` returns nil) |
| `load_new_symbols` | `loadNewSymbols` function does not exist in this CE version |
| `pointer_rescan` | `pointerRescan` function does not exist in this CE version |
| `inject_dotnet_dll` | `injectDotNetDLL` function does not exist |

### Tested Tool Categories

<details>
<summary><b>Click to expand full test results by category</b></summary>

| Category | Tools Tested (all passed) |
|----------|--------------------------|
| **Basic** | `ping`, `get_process_info`, `get_opened_process_id`, `get_opened_process_handle`, `get_processid_from_name`, `get_foreground_process` |
| **Process** | `open_process`, `get_process_list`, `create_process`, `get_thread_list`, `pause_process`, `unpause_process` |
| **Modules/Symbols** | `enum_modules`, `get_module_size`, `get_symbol_address`, `get_symbol_info`, `enum_registered_symbols`, `register_symbol`, `unregister_symbol`, `delete_all_registered_symbols`, `reinitialize_symbol_handler`, `enable_kernel_symbols`, `enable_windows_symbols` |
| **Memory Info** | `get_memory_regions`, `get_memory_protection`, `enum_memory_regions_full`, `get_instruction_info` |
| **Memory Read** | `read_memory`, `read_integer`, `read_string`, `read_pointer`, `read_pointer_chain` |
| **Memory Write** | `write_memory`, `write_integer`, `write_string` |
| **Memory Mgmt** | `allocate_memory`, `free_memory`, `allocate_shared_memory`, `set_memory_protection`, `full_access`, `checksum_memory`, `md5_memory`, `compare_memory`, `copy_memory`, `map_memory`, `unmap_memory`, `create_section` |
| **Scanning** | `aob_scan`, `aob_scan_module`, `aob_scan_module_unique`, `aob_scan_unique`, `search_string`, `scan_all`, `next_scan`, `get_scan_results` |
| **Persistent Scan** | `create_persistent_scan`, `persistent_scan_first_scan`, `persistent_scan_next_scan`, `persistent_scan_get_results`, `persistent_scan_destroy` |
| **Analysis** | `disassemble`, `assemble_instruction`, `auto_assemble`, `auto_assemble_check`, `analyze_function`, `find_function_boundaries`, `find_references`, `find_call_references`, `get_address_info`, `get_rtti_classname`, `generate_signature`, `dissect_structure`, `generate_code_injection_script`, `generate_api_hook_script` |
| **Structures** | `create_structure`, `get_structure_by_name`, `add_element_to_structure`, `get_structure_elements`, `export_structure_to_xml`, `delete_structure` |
| **Memory Records** | `create_memory_record`, `get_memory_record`, `get_memory_record_value`, `set_memory_record_value`, `delete_memory_record`, `get_address_list` |
| **Debug (safe)** | `list_breakpoints`, `clear_all_breakpoints`, `debug_is_debugging`, `debug_get_current_debugger_interface` |
| **Lua/Script** | `evaluate_lua`, `execute_code`, `execute_code_ex`, `create_thread`, `queue_to_main_thread`, `in_main_thread`, `check_synchronize` |
| **File Ops** | `get_file_list`, `get_directory_list`, `get_file_version`, `file_exists`, `delete_file`, `md5_file`, `get_temp_folder`, `write_region_to_file`, `read_region_from_file`, `save_table`, `load_table` |
| **GUI/Input** | `show_message`, `speak_text`, `beep`, `play_sound`, `input_query`, `is_key_pressed`, `key_down`, `key_up`, `do_key_press`, `get_mouse_pos`, `set_mouse_pos`, `get_pixel`, `get_screen_info`, `set_progress_state`, `set_progress_value`, `read_clipboard`, `write_clipboard`, `output_debug_string`, `send_window_message` |
| **Variables** | `set_global_variable`, `get_global_variable` |
| **Kernel (basic)** | `dbk_get_cr0`, `dbk_get_cr3`, `dbk_get_cr4`, `get_physical_address`, `dbk_writes_ignore_write_protection` |

</details>

---

## Changelog

### v15.0.0 — Native DLL TCP Bridge
- Replaced Winsock FFI with compiled C DLL for all TCP I/O
- Dynamic Lua API resolution (cross-CE-version compatible)
- Static CRT linking (`/MT`) — no VC runtime redistributable
- Architecture-specific DLLs (`_x64.dll` / `_x86.dll`)
- Dedicated DLL debug console for diagnostics
- Eliminated all kernel32 FFI and PEB-walk issues
- Removed ~1000 lines of dead FFI/Winsock/Pipe code from Lua

### v14.1.0 — TCP Transport
- Winsock FFI TCP server in CE Lua (default transport)
- Remote CE support via `CE_HOST`
- Port auto-increment (17171–17181)
- 90-second default timeout

### v12.0.0 — Initial Release (Original)
- Named Pipe transport with `pywin32`
- ~180 MCP tools
- Optional TCP relay script (`ce_tcp_relay.py`)

---

## Credits

This project is a TCP-enhanced fork of:

- **Original**: [miscusi-peek/cheatengine-mcp-bridge](https://github.com/miscusi-peek/cheatengine-mcp-bridge) by [@miscusi-peek](https://github.com/miscusi-peek)
- **Contributors**: [@libangli218](https://github.com/libangli218), [@lauralex](https://github.com/lauralex), [@iamtyroon](https://github.com/iamtyroon)

---

## Disclaimer

This code is for educational and research purposes only. It demonstrates the capabilities of the Model Context Protocol (MCP) and LLM-based debugging. Do not use these tools for malicious hacking, cheating in multiplayer games, or violating Terms of Service.
