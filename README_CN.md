**[English](README.md) | [中文](README_CN.md)**

# Cheat Engine MCP Bridge — TCP 增强版

[![Version](https://img.shields.io/badge/version-15.0.0-blue.svg)](#) [![Python](https://img.shields.io/badge/python-3.10%2B-green.svg)](https://python.org) [![Transport](https://img.shields.io/badge/transport-原生%20TCP%20DLL-orange.svg)](#)

> 基于 [miscusi-peek/cheatengine-mcp-bridge](https://github.com/miscusi-peek/cheatengine-mcp-bridge) 的高性能分支。用原生 C TCP 桥接替代了 Windows 命名管道传输，开箱即用地支持**远程 CE 控制**、**零 pywin32 依赖**和**多实例并行**。

[演示视频](https://github.com/user-attachments/assets/a184a006-f569-4b55-858a-ed80a7139035)

---

## 与原版的核心差异

本分支从原版 v12.0.0 开始，在传输层、架构和工程实践上做了重大改造。以下是逐项对比。

### 架构对比

```
原版 (v12.0.0)                              本分支 (v15.0.0)
────────────                               ────────────
AI 客户端                                    AI 客户端
  │ stdio JSON-RPC                            │ stdio JSON-RPC
  ▼                                           ▼
mcp_cheatengine.py                          mcp_cheatengine.py
  │ 命名管道 (需要 pywin32)                     │ TCP 套接字 (仅 stdlib)
  ▼                                           ▼
\\.\pipe\CE_MCP_Bridge_v99                  ce_mcp_tcp.dll (原生 C)
  │                                           │ Winsock2 + select()
  ▼                                           ▼
ce_mcp_bridge.lua                           ce_mcp_bridge.lua
  │ Worker 线程 (Lua 管道 I/O)                  │ 1ms 定时器轮询
  ▼                                           ▼
目标进程                                      目标进程
```

### 功能对比表

| 特性 | 原版 (v12.0.0) | 本分支 (v15.0.0) |
|------|:--------------:|:----------------:|
| **传输方式** | Windows 命名管道 | 原生 TCP（编译的 C DLL） |
| **远程 CE 支持** | 需额外 `ce_tcp_relay.py` 中继脚本 | 内置（`CE_HOST` 环境变量） |
| **Python 依赖** | `mcp` + `pywin32` | 仅 `mcp` |
| **多实例支持** | 不支持 | 端口自动递增（17171–17181） |
| **默认超时** | 30 秒 | 90 秒 |
| **CE Lua I/O 模型** | Worker 线程阻塞管道 | 1ms 定时器通过 DLL 轮询 |
| **原生代码** | 无 | `ce_mcp_tcp_x64.dll` / `ce_mcp_tcp_x86.dll` |
| **DLL 调试控制台** | 无 | 独立诊断控制台窗口 |
| **安装复杂度** | 仅 `pip install` | `pip install` + 复制 DLL 到 CE 目录 |
| **跨平台 MCP 服务器** | 需要中继脚本 | 原生支持（TCP 用 stdlib） |
| **Lua 代码量** | ~6700 行（含 FFI/Winsock/Pipe 代码） | ~5700 行（删除 1000 行废弃代码） |
| **CRT 依赖** | 无 | 静态 `/MT` 链接——无需 VC 运行库 |
| **中文文档** | 无 | 有 |

### 我们获得了什么

- **真正的远程调试** — 将 `CE_HOST` 指向运行 CE 的任意机器。无需中继脚本、无需额外进程。
- **去掉 pywin32** — 默认 TCP 传输仅使用 Python 标准库。安装更简单、故障点更少。
- **多 CE 实例支持** — 同时运行多个 CE 实例；每个自动分配独立端口（17171、17172……）。Python 客户端通过 `ping` 验证自动发现正确的实例。
- **原生 C 性能** — TCP I/O 由编译的 DLL 处理，不再依赖 Lua FFI。消除了 `getAddressSafe` 崩溃、PEB 遍历失败、kernel32 FFI 问题。
- **更高可靠性** — 90 秒默认超时（原版 30 秒）、3 次自动重试 + 自动重连、线程安全锁。
- **DLL 调试控制台** — 专用控制台窗口实时显示 TCP 状态、Lua API 解析、连接事件。故障排查一目了然。
- **更干净的 Lua 代码** — 删除了约 1000 行废弃的 Winsock FFI、管道 Worker 线程和 kernel32 引导代码。

### 我们失去了什么

- **零 DLL 简洁性** — 原版只需安装 Python 包。本分支需要将 DLL 放在 CE 可执行文件旁（多一步操作）。
- **内置 TCP 中继** — 原版包含 `ce_tcp_relay.py` 用于将命名管道桥接到 TCP。本分支不需要它（TCP 已原生），但中继脚本本身被移除了。
- **管道传输** — 仍可通过 `CE_TRANSPORT=pipe` + `pywin32` 使用，但已弃用，不再是默认选项。

### 安全提示

TCP 桥接**没有身份验证和加密**。原版的命名管道天然仅限本机访问，提供了隐式访问控制。使用 TCP 后，端口可通过网络访问。**仅在可信网络（VPN、局域网）中暴露。绝不要将端口 17171 开放到公共互联网。**

---

## 项目功能

将 AI 代理（Claude、Cursor、Copilot、Codex）通过模型上下文协议（MCP）连接到 Cheat Engine。AI 可以读写内存、扫描数值、设置硬件断点、反汇编函数、注入代码，以及对任何附加进程执行约 180 种操作——全部通过自然语言完成。

```
你："扫描金币：15000"           →  AI 找到 47 个结果
你："金币变成了 15100"          →  AI 过滤到 3 个地址
你："什么写入了第一个？"         →  AI 设置硬件断点
你："反汇编那个函数"            →  完整的 AddGold 逻辑呈现
```

---

## 前置条件

| 条件 | 版本 | 说明 |
|------|------|------|
| Python | 3.10+ | MCP 服务器运行环境 |
| Cheat Engine | 7.5+ | 推荐 7.6；DBVM 功能需要 DBVM 版本 |
| 原生 TCP DLL | v2.0.0 | `ce_mcp_tcp_x64.dll` 或 `_x86.dll`——须匹配 CE 架构 |
| pip 包 `mcp` | 最新版 | `pip install mcp` |

> `pywin32` 仅在使用旧版命名管道模式（`CE_TRANSPORT=pipe`）时需要。TCP 模式（默认）无额外依赖。

---

## 安装配置

### 1. 克隆与安装

```bash
git clone https://github.com/HollyZoe/cheatengine-mcp-tcp-bridge.git
cd cheatengine-mcp-tcp-bridge
pip install -r MCP_Server/requirements.txt
```

### 2. 放置 DLL

将架构匹配的 DLL 复制到 **Cheat Engine 可执行文件目录**：

| CE 版本 | 需复制的 DLL |
|---------|-------------|
| 64 位 | `ce_mcp_tcp_x64.dll` |
| 32 位 | `ce_mcp_tcp_x86.dll` |

来源：`MCP_Server/` 或 `NativeBridge/bin/x64/`、`NativeBridge/bin/x86/`。

```
C:\CE 7.5\cheatengine-x86_64.exe
C:\CE 7.5\ce_mcp_tcp_x64.dll    ← 放在这里
```

### 3. 在 Cheat Engine 中加载

1. 将 CE 附加到目标进程。
2. 加载桥接脚本：
   - **推荐：** `File` → `Execute Script` → 打开 `MCP_Server/ce_mcp_bridge.lua` → `Execute`
   - **备选：** `Table` → `Show Cheat Table Lua Script` → 粘贴执行：
     ```lua
     dofile([[C:\path\to\cheatengine-mcp-tcp-bridge\MCP_Server\ce_mcp_bridge.lua]])
     ```

预期输出：
```
[MCP] CE x64 - loading ce_mcp_tcp_x64.dll
[MCP] DLL loaded OK from: C:\CE 7.5\ce_mcp_tcp_x64.dll
[MCP] Bridge started on port 17171 (native TCP, 1ms poll)
```

### 4. 配置 AI 客户端

<details>
<summary><b>Cursor IDE</b></summary>

`.cursor/mcp.json`：
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
保存后重启 Cursor。
</details>

<details>
<summary><b>Claude Desktop</b></summary>

`%APPDATA%\Claude\claude_desktop_config.json`：
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

`~/.codex/config.toml`：
```toml
[mcp_servers.cheatengine]
command = "python"
args = ['C:\path\to\cheatengine-mcp-tcp-bridge\MCP_Server\mcp_cheatengine.py']
```
</details>

<details>
<summary><b>远程 Cheat Engine</b></summary>

将 `CE_HOST` 设为远程 IP：
```json
{
  "env": {
    "CE_HOST": "192.168.1.100",
    "CE_PORT": "17171"
  }
}
```

CE 所在机器防火墙：
```powershell
netsh advfirewall firewall add rule name="CE MCP Bridge" dir=in action=allow protocol=TCP localport=17171
```
</details>

### 5. 验证连接

对 AI 说：*"Ping 一下 Cheat Engine"*

```json
{"success": true, "version": "15.0.0", "message": "CE MCP Bridge v15.0.0 alive"}
```

---

## 可用工具（约 180 个）

### 内存操作
| 工具 | 说明 |
|------|------|
| `read_memory`, `read_integer`, `read_string` | 读取任意数据类型 |
| `write_memory`, `write_integer`, `write_string` | 写入内存值 |
| `read_pointer_chain` | 跟踪多级指针链 `[[base+0x10]+0x20]` |
| `scan_all`, `next_scan`, `aob_scan` | 数值扫描和 AOB 特征码匹配 |
| `allocate_memory`, `free_memory` | 在目标进程中分配/释放内存 |

### 代码分析
| 工具 | 说明 |
|------|------|
| `disassemble` | 反汇编指定地址的指令 |
| `analyze_function` | 查找函数内所有 CALL 指令 |
| `find_function_boundaries` | 检测函数起止地址 |
| `dissect_structure` | 自动检测结构体字段和类型 |
| `get_rtti_classname` | 通过 RTTI 识别 C++ 对象类型 |
| `find_references` | 查找引用某地址的指令 |

### 调试
| 工具 | 说明 |
|------|------|
| `set_breakpoint`, `set_data_breakpoint` | 硬件断点（DR0–DR3） |
| `start_dbvm_watch` | 通过 DBVM 实现 Ring -1 隐形追踪 |
| `get_breakpoint_hits` | 获取捕获的寄存器/栈数据 |

### 代码注入
| 工具 | 说明 |
|------|------|
| `auto_assemble` | 执行 CE 自动汇编器脚本 |
| `inject_dll` | 向目标进程注入 DLL |
| `execute_code`, `execute_method` | 远程执行 shellcode 或调用方法 |
| `compile_c_code` | 编译 C 源码为可注入的 shellcode |

### 进程与模块
| 工具 | 说明 |
|------|------|
| `get_process_info`, `get_process_list` | 进程枚举与信息 |
| `open_process`, `create_process` | 附加或启动进程 |
| `enum_modules`, `get_symbol_address` | 模块和符号解析 |

### 内核模式（DBK / DBVM）
| 工具 | 说明 |
|------|------|
| `dbk_get_cr3`, `dbk_get_cr0` | 读取控制寄存器 |
| `read_process_memory_cr3` | 通过 CR3 绕过读取物理内存 |
| `get_physical_address` | 虚拟地址到物理地址转换 |

### 其他分类
- **符号管理**：`register_symbol`、`get_symbol_info`、`enable_windows_symbols`
- **汇编**：`assemble_instruction`、`generate_api_hook_script`
- **GUI 自动化**：`find_window`、`send_window_message`
- **输入**：`get_pixel`、`is_key_pressed`、`do_key_press`、`set_mouse_pos`
- **作弊表**：`load_table`、`save_table`、`get_address_list`、`create_memory_record`
- **文件/系统**：`file_exists`、`run_command`、`shell_execute`
- **结构体**：`create_structure`、`add_element_to_structure`、`export_structure_to_xml`

完整参考：`AI_Context/MCP_Bridge_Command_Reference.md`

---

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `CE_TRANSPORT` | `tcp` | `tcp`（推荐）或 `pipe`（旧版，需 pywin32） |
| `CE_HOST` | `127.0.0.1` | CE 实例 IP。远程调试时设为远程 IP |
| `CE_PORT` | `17171` | 基础 TCP 端口。DLL 在端口被占用时自动递增 |
| `CE_PORT_RANGE` | `10` | 从 `CE_PORT` 起扫描的端口数量 |
| `CE_MCP_TIMEOUT` | `90` | 每个工具调用超时（秒）。`0` = 不超时 |
| `CE_MCP_ALLOW_SHELL` | *(未设置)* | 设为 `1` 启用 `run_command`/`shell_execute`。有安全风险 |

---

## 技术架构

### 原生 TCP DLL (`ce_mcp_tcp.c`)

DLL（编译的 C 代码，静态 CRT）由 CE Lua 通过 `package.loadlib` 加载：

1. 动态解析 CE 的 `lua53-64.dll` 中的 17 个 Lua C API 函数
2. 注册 5 个全局 Lua 函数：`mcp_tcp_start/stop/poll/respond/status`
3. 运行专用 TCP 服务端线程（Winsock2 + `select()`）
4. 绑定 `0.0.0.0:17171`，端口忙时自动递增
5. 将传入的 JSON-RPC 请求入队，Lua 通过 `mcp_tcp_poll()` 出队
6. 通过 `mcp_tcp_respond()` 发送响应，使用 4 字节小端序长度前缀帧

### 线路协议

TCP 和旧版管道使用相同的帧格式：

```
[4 字节：载荷长度，小端序 uint32] [N 字节：UTF-8 JSON-RPC]
```

### Python 客户端行为

- 自动扫描 17171–17180 端口，通过 `ping` 验证每个端口
- 失败时 3 次自动重试 + 自动重连
- 通过 `threading.Lock()` 保证线程安全
- 可配置超时，自动清理 Socket

---

## 重要配置

> **防止蓝屏**：**必须**禁用 Cheat Engine → Settings → Extra → **"Query memory region routines"**。启用时，对 DBVM 保护页面的内存扫描会触发 `CLOCK_WATCHDOG_TIMEOUT` 蓝屏。

---

## 故障排除

| 问题 | 解决方案 |
|------|---------|
| CE 报 "too many local variables" | 用 `dofile(...)` 从磁盘加载，不要粘贴完整脚本 |
| DLL 未找到 | 将 `ce_mcp_tcp_x64.dll`（或 `_x86.dll`）复制到 CE 可执行文件目录 |
| Lua API 解析失败 | 检查 DLL 调试控制台的模块检测信息。确保 CE 7.5+ 且为 Lua 5.3 构建 |
| 无法连接（TCP） | 验证 `netstat -an | findstr 17171`、检查 CE_HOST/CE_PORT、重启 IDE |
| 无法连接（Pipe/旧版） | `pip install pywin32`，设置 `CE_TRANSPORT=pipe`，确认 CE 输出中的管道名 |
| 高负载操作超时 | 增大 `CE_MCP_TIMEOUT`（如 `180`） |
| CE UI 在命令执行时卡顿 | 正常现象——处理器在 CE 主线程运行以保证 API 安全 |

---

## 项目结构

```
README.md / README_CN.md          文档（英/中）
CLAUDE.md / AGENTS.md             AI 代理编码指南

MCP_Server/
├── mcp_cheatengine.py            Python MCP 服务器（约 2350 行）
├── ce_mcp_bridge.lua             CE Lua 桥接（约 5700 行）
├── ce_mcp_tcp_x64.dll            预编译 64 位原生 TCP DLL
├── ce_mcp_tcp_x86.dll            预编译 32 位原生 TCP DLL
├── test_mcp.py                   集成测试套件
└── requirements.txt              Python 依赖

NativeBridge/
├── ce_mcp_tcp.c                  DLL 源码（约 770 行）
├── build.bat                     编译脚本（VS Build Tools）
└── bin/{x64,x86}/                预编译二进制

AI_Context/
├── MCP_Bridge_Command_Reference.md
├── AI_Guide_MCP_Server_Implementation.md
├── CE_LUA_Documentation.md
└── BATCH_WORKER_BRIEFING.md
```

---

## 接口测试结果 (v15.0.0)

对所有 161 个已注册的 MCP 工具进行了完整的接口测试。结果汇总：

| 类别 | 结果 | 数量 |
|------|------|------|
| **通过** | 所有操作返回预期结果 | **110+** |
| **测试中修复** | 发现并修补的 Bug | **3** |
| **CE 环境限制** | 当前 CE 版本不支持的函数 | **5** |
| **需要内核驱动** | 已跳过（需要签名驱动 / DBVM） | **约 20** |
| **危险操作（需正确参数）** | 功能正常但传入无效地址会导致 CE 崩溃 | **3** |

### 测试中修复的 Bug

| 工具 | 问题 | 修复方案 |
|------|------|---------|
| `get_memory_protection` | 调用了不存在的 `getMemoryProtection()` CE 函数 | 改用 `enumMemoryRegions()` 遍历内存区域查找包含地址的区域 |
| `get_memory_regions` | 同一根因——返回 0 个区域 | 改用 `enumMemoryRegions()` 实现，现在返回所有已提交的内存区域并支持分页 |
| `debug_get_current_debugger_interface` | 无安全检查就调用 `debug_getCurrentDebuggerInterface()`，导致 CE 崩溃 | 添加 nil 检查和 `debug_isDebugging()` 前置判断 |

### CE 环境限制（非 Bug）

| 工具 | 原因 |
|------|------|
| `compile_c_code` | TCC 编译器库（`tcc64-64.dll`）未安装 |
| `compile_cs_code` | .NET 编译器不可用（`compileCS` 返回 nil） |
| `load_new_symbols` | 当前 CE 版本不存在 `loadNewSymbols` 函数 |
| `pointer_rescan` | 当前 CE 版本不存在 `pointerRescan` 函数 |
| `inject_dotnet_dll` | 不存在 `injectDotNetDLL` 函数 |

### 按类别的测试详情

<details>
<summary><b>点击展开各类别完整测试结果</b></summary>

| 类别 | 测试通过的工具 |
|------|--------------|
| **基础连接** | `ping`, `get_process_info`, `get_opened_process_id`, `get_opened_process_handle`, `get_processid_from_name`, `get_foreground_process` |
| **进程管理** | `open_process`, `get_process_list`, `create_process`, `get_thread_list`, `pause_process`, `unpause_process` |
| **模块/符号** | `enum_modules`, `get_module_size`, `get_symbol_address`, `get_symbol_info`, `enum_registered_symbols`, `register_symbol`, `unregister_symbol`, `delete_all_registered_symbols`, `reinitialize_symbol_handler`, `enable_kernel_symbols`, `enable_windows_symbols` |
| **内存信息** | `get_memory_regions`, `get_memory_protection`, `enum_memory_regions_full`, `get_instruction_info` |
| **内存读取** | `read_memory`, `read_integer`, `read_string`, `read_pointer`, `read_pointer_chain` |
| **内存写入** | `write_memory`, `write_integer`, `write_string` |
| **内存管理** | `allocate_memory`, `free_memory`, `allocate_shared_memory`, `set_memory_protection`, `full_access`, `checksum_memory`, `md5_memory`, `compare_memory`, `copy_memory`, `map_memory`, `unmap_memory`, `create_section` |
| **扫描** | `aob_scan`, `aob_scan_module`, `aob_scan_module_unique`, `aob_scan_unique`, `search_string`, `scan_all`, `next_scan`, `get_scan_results` |
| **持久扫描** | `create_persistent_scan`, `persistent_scan_first_scan`, `persistent_scan_next_scan`, `persistent_scan_get_results`, `persistent_scan_destroy` |
| **分析** | `disassemble`, `assemble_instruction`, `auto_assemble`, `auto_assemble_check`, `analyze_function`, `find_function_boundaries`, `find_references`, `find_call_references`, `get_address_info`, `get_rtti_classname`, `generate_signature`, `dissect_structure`, `generate_code_injection_script`, `generate_api_hook_script` |
| **结构体** | `create_structure`, `get_structure_by_name`, `add_element_to_structure`, `get_structure_elements`, `export_structure_to_xml`, `delete_structure` |
| **内存记录** | `create_memory_record`, `get_memory_record`, `get_memory_record_value`, `set_memory_record_value`, `delete_memory_record`, `get_address_list` |
| **调试（安全）** | `list_breakpoints`, `clear_all_breakpoints`, `debug_is_debugging`, `debug_get_current_debugger_interface` |
| **Lua/脚本** | `evaluate_lua`, `execute_code`, `execute_code_ex`, `create_thread`, `queue_to_main_thread`, `in_main_thread`, `check_synchronize` |
| **文件操作** | `get_file_list`, `get_directory_list`, `get_file_version`, `file_exists`, `delete_file`, `md5_file`, `get_temp_folder`, `write_region_to_file`, `read_region_from_file`, `save_table`, `load_table` |
| **GUI/输入** | `show_message`, `speak_text`, `beep`, `play_sound`, `input_query`, `is_key_pressed`, `key_down`, `key_up`, `do_key_press`, `get_mouse_pos`, `set_mouse_pos`, `get_pixel`, `get_screen_info`, `set_progress_state`, `set_progress_value`, `read_clipboard`, `write_clipboard`, `output_debug_string`, `send_window_message` |
| **变量** | `set_global_variable`, `get_global_variable` |
| **内核（基础）** | `dbk_get_cr0`, `dbk_get_cr3`, `dbk_get_cr4`, `get_physical_address`, `dbk_writes_ignore_write_protection` |

</details>

---

## 更新日志

### v15.0.0 — 原生 DLL TCP 桥接
- 用编译的 C DLL 替代 Winsock FFI 处理所有 TCP I/O
- 动态 Lua API 解析（跨 CE 版本兼容）
- 静态 CRT 链接（`/MT`）——无需 VC 运行库再发行包
- 架构专用 DLL（`_x64.dll` / `_x86.dll`）
- 专用 DLL 调试控制台
- 消除所有 kernel32 FFI 和 PEB 遍历问题
- 从 Lua 中删除约 1000 行废弃 FFI/Winsock/Pipe 代码

### v14.1.0 — TCP 传输
- CE Lua 中基于 Winsock FFI 的 TCP 服务器（默认传输）
- 通过 `CE_HOST` 支持远程 CE
- 端口自动递增（17171–17181）
- 90 秒默认超时

### v12.0.0 — 初始版本（原版）
- 命名管道传输 + `pywin32`
- 约 180 个 MCP 工具
- 可选 TCP 中继脚本（`ce_tcp_relay.py`）

---

## 致谢

本项目是以下项目的 TCP 增强分支：

- **原版**：[miscusi-peek/cheatengine-mcp-bridge](https://github.com/miscusi-peek/cheatengine-mcp-bridge)，作者 [@miscusi-peek](https://github.com/miscusi-peek)
- **贡献者**：[@libangli218](https://github.com/libangli218)、[@lauralex](https://github.com/lauralex)、[@iamtyroon](https://github.com/iamtyroon)

---

## 免责声明

本代码仅用于教育和研究目的，旨在展示模型上下文协议（MCP）和基于 LLM 的调试能力。请勿将这些工具用于恶意攻击、多人游戏作弊或违反服务条款。
