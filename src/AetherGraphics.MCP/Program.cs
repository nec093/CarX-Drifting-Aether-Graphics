using AetherGraphics.Mcp.Ipc;
using AetherGraphics.Mcp.Server;
using AetherGraphics.Mcp.Tools;

// Aether MCP server: stdio on one side, the game's local pipe on the other.
//   AetherGraphics.MCP.exe [--allow-control] [--pipe <name>] [--list-tools]
// --allow-control   offer the [control] tools (the game still applies its own rule: deny / ask / allow)
// --pipe <name>     pipe name without the \\.\pipe\ prefix (default AetherGraphics.Runtime, or AETHER_GRAPHICS_PIPE_NAME)

bool allowControl = Environment.GetEnvironmentVariable("AETHER_MCP_ALLOW_CONTROL") is "1" or "true";
string pipe = Environment.GetEnvironmentVariable("AETHER_GRAPHICS_PIPE_NAME") is { Length: > 0 } env ? env : "AetherGraphics.Runtime";
for (int i = 0; i < args.Length; ++i) {
	switch (args[i]) {
	case "--allow-control":
		allowControl = true;
		break;
	case "--pipe" when i + 1 < args.Length:
		pipe = args[++i];
		break;
	case "--list-tools":
		foreach (Tool t in ToolCatalog.All) {
			Console.WriteLine($"{t.Name}{(t.Control ? " [control]" : "")}");
		}
		return 0;
	case "--help" or "-h":
		Console.WriteLine("AetherGraphics.MCP [--allow-control] [--pipe <name>] [--list-tools]");
		return 0;
	default:
		Console.Error.WriteLine("[aether-mcp] unknown argument: " + args[i]);
		return 2;
	}
}

Console.Error.WriteLine($"[aether-mcp] pipe \\\\.\\pipe\\{pipe}, control tools {(allowControl ? "offered" : "off (read-only)")}");
using var cancel = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => {
	e.Cancel = true;
	cancel.Cancel();
};
using var client = new RuntimeClient(pipe, allowControl);
try {
	await new McpServer(client).Run(cancel.Token);
} catch (OperationCanceledException) {
}
return 0;
