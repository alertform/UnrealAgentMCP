#pragma once

namespace AgentMcp::Tools
{
	/** Registers UMG widget-tree authoring tools (add_widget, list_widgets, set_widget_property). */
	void RegisterWidgetTools();

	/** Registers rename_widget and add_component_event tools. Split from WidgetTools to stay under the 800-line cap. */
	void RegisterWidgetEditTools();
}
