#pragma once

#include "CoreMinimal.h"
#include "Core/AgentMcpTier.h"
#include "Dom/JsonObject.h"

namespace AgentMcp
{
	/** Single source of truth for the plugin version reported by initialize and engine_info. */
	inline constexpr const TCHAR* PluginVersion = TEXT("1.1.0");
}

/** Outcome of one tool invocation. Text carries the payload (JSON string or plain message). */
struct FAgentMcpToolResult
{
	bool bIsError = false;
	FString Text;

	static FAgentMcpToolResult Success(const FString& InText)
	{
		FAgentMcpToolResult Result;
		Result.Text = InText;
		return Result;
	}

	static FAgentMcpToolResult Error(const FString& InMessage)
	{
		FAgentMcpToolResult Result;
		Result.bIsError = true;
		Result.Text = InMessage;
		return Result;
	}
};

/** Tool handler. Arguments may be nullptr when the client sends no `arguments` object. Runs on game thread. */
DECLARE_DELEGATE_RetVal_OneParam(FAgentMcpToolResult, FAgentMcpToolHandler, const TSharedPtr<FJsonObject>&);

/** Static definition of one MCP tool. */
struct FAgentMcpToolDef
{
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	EAgentMcpTier Tier = EAgentMcpTier::ReadOnly;
	FAgentMcpToolHandler Handler;
};
