#pragma once

#include "CoreMinimal.h"
#include "AgentMcpTier.generated.h"

/** Permission tier of an MCP tool. Calls above the configured ceiling are rejected (enforcement lands in P3). */
UENUM()
enum class EAgentMcpTier : uint8
{
	/** Pure queries. Never mutates editor state. */
	ReadOnly = 0,
	/** Mutations wrapped in editor transactions (undoable via Ctrl+Z). */
	SafeWrite = 1,
	/** Irreversible or disk-level operations (delete asset, arbitrary console command). */
	Destructive = 2,
};

namespace AgentMcp
{
	/** Display name for tier comparisons in error messages and audit lines. */
	inline const TCHAR* TierToString(EAgentMcpTier Tier)
	{
		switch (Tier)
		{
		case EAgentMcpTier::ReadOnly:    return TEXT("ReadOnly");
		case EAgentMcpTier::SafeWrite:   return TEXT("SafeWrite");
		case EAgentMcpTier::Destructive: return TEXT("Destructive");
		default:                         return TEXT("Unknown");
		}
	}
}
