#pragma once

#include "CoreMinimal.h"

/**
 * One audited tool invocation (or tier rejection).
 * Spec §5 lists a transaction-name field; deliberately omitted — transaction names are mechanically
 * derived from tool names ("MCP: Add Node" <-> add_node), so Tool already disambiguates.
 */
struct FAgentMcpAuditEntry
{
	FString Tool;
	FString ArgsDigest;
	bool bIsError = false;
	bool bRejectedByTier = false;
	double DurationMs = 0.0;
};

/**
 * Append-only JSONL audit trail under Saved/AgentMCP/audit-YYYYMMDD.jsonl.
 * Thread-safe appends; honors UAgentMcpSettings::bEnableAuditLog (Append no-ops when disabled).
 */
class UNREALAGENTMCP_API FAgentMcpAuditLog
{
public:
	static FAgentMcpAuditLog& Get();

	void Append(const FAgentMcpAuditEntry& Entry);

	/**
	 * Last Count lines of today's file (audit-YYYYMMDD.jsonl), oldest-first. Empty when the file
	 * doesn't exist. Entries written before a midnight rollover live in the previous day's file
	 * and are NOT returned here.
	 */
	TArray<FString> Tail(int32 Count) const;

	/** Full path of today's audit file (exists only after the first Append). */
	FString CurrentFilePath() const;

private:
	mutable FCriticalSection FileLock;
};
