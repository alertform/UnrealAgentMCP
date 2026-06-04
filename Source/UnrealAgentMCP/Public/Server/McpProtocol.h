#pragma once

#include "CoreMinimal.h"

namespace AgentMcp::Protocol
{
	/**
	 * Handles one JSON-RPC 2.0 message (MCP streamable-HTTP, single-message mode).
	 * Returns the serialized JSON response, or an empty string for notifications
	 * (transport must answer HTTP 202 with an empty body in that case).
	 * Must be called on the game thread.
	 */
	UNREALAGENTMCP_API FString HandleMessage(const FString& RequestBody);
}
