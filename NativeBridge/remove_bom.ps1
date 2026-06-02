$path = "C:\Users\Admin\Desktop\WorkSpace\MCPs\cheatengine-mcp-bridge\MCP_Server\ce_mcp_bridge.lua"
$raw = [System.IO.File]::ReadAllBytes($path)
if ($raw[0] -eq 0xEF -and $raw[1] -eq 0xBB -and $raw[2] -eq 0xBF) {
    $trimmed = $raw[3..($raw.Length - 1)]
    [System.IO.File]::WriteAllBytes($path, [byte[]]$trimmed)
    Write-Host "BOM removed"
} else {
    Write-Host "No BOM"
}
