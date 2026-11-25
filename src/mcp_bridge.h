/**
 * @file src/mcp_bridge.h
 * @brief "tunastream --mcp": TunaStream's tools for AI assistants (MCP over standard input/output).
 */
#pragma once

namespace mcp_bridge {
  /// Run the MCP server until standard input closes. Uses TUNASTREAM_TOKEN.
  int run(int argc, char *argv[]);
}  // namespace mcp_bridge
