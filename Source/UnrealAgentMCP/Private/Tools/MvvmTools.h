#pragma once

namespace AgentMcp::Tools
{
	/** Registers MVVM viewmodel authoring tools (add_viewmodel). */
	void RegisterMvvmTools();

	/** Registers MVVM view-binding tools (add_view_binding, list_view_bindings, remove_view_binding). Split from MvvmTools to stay under the 800-line cap. */
	void RegisterMvvmBindingTools();
}
