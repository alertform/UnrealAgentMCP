#pragma once

#include "CoreMinimal.h"
#include "Core/McpTypes.h"

class FJsonValue;

/**
 * Registry of all MCP tools. Instantiable for tests; the live instance is FAgentMcpToolRegistry::Get().
 * Not thread-safe by design: registration happens at module startup, lookups on the game thread.
 */
class UNREALAGENTMCP_API FAgentMcpToolRegistry
{
public:
	/** Live registry used by the protocol layer. */
	static FAgentMcpToolRegistry& Get();

	/** Registers a tool. Re-registering the same name overwrites (last wins). */
	void Register(FAgentMcpToolDef&& Def);

	/** nullptr when no tool has that name. */
	const FAgentMcpToolDef* Find(const FString& Name) const;

	/** MCP tools/list payload: array of {name, description, inputSchema}. */
	TArray<TSharedPtr<FJsonValue>> BuildToolsJson() const;

	int32 Num() const { return Tools.Num(); }

private:
	TMap<FString, FAgentMcpToolDef> Tools;
};
