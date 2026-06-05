#pragma once

namespace AgentMcp::Tools
{
	/** Registers Blueprint node-graph editing tools (read_graph, add_node, connect_pins, delete_node). */
	void RegisterNodeGraphTools();

	/** Registers set_pin_default and auto_layout tools (split from NodeGraphTools to stay under the 800-line cap). */
	void RegisterGraphPinAndLayoutTools();
}
