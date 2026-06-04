#pragma once

#include "CoreMinimal.h"
#include "Core/McpTypes.h"
#include "Dom/JsonValue.h"

/**
 * Registry of all MCP tools. Instantiable for tests; the live instance is FAgentMcpToolRegistry::Get().
 * Not thread-safe by design: registration happens at module startup, lookups on the game thread.
 */
class UNREALAGENTMCP_API FAgentMcpToolRegistry
{
public:
	/** Live registry used by the protocol layer. */
	static FAgentMcpToolRegistry& Get();

	/** Registers a tool. Re-registering the same name overwrites (last wins). Invalidates all pointers previously returned by Find().
	 *  Must not be called while a tool handler is executing (the dispatch seam holds a raw Find() pointer across Execute). */
	void Register(FAgentMcpToolDef&& Def);

	/**
	 * Returns a pointer into the internal map, or nullptr when no tool has that name.
	 * Valid only until the next Register() call — call Find() per request, never cache across registrations.
	 */
	const FAgentMcpToolDef* Find(const FString& Name) const;

	/** MCP tools/list payload: array of {name, description, inputSchema}. */
	TArray<TSharedPtr<FJsonValue>> BuildToolsJson() const;

	int32 Num() const { return Tools.Num(); }

private:
	TMap<FString, FAgentMcpToolDef> Tools;
};
