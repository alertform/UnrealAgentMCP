#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Ring buffer of recent GLog lines, installed by the module at startup. Thread-safe: logs arrive
 * from any thread; reads happen on the game thread via the read_output_log tool.
 */
class FAgentMcpLogCapture final : public FOutputDevice
{
public:
	static constexpr int32 MaxLines = 2000;

	FAgentMcpLogCapture();

	static FAgentMcpLogCapture& Get();

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** Last Count matching lines, oldest-first. Filter = case-insensitive substring; Category = exact log category name. Empty strings = no filter. */
	TArray<FString> Recent(int32 Count, const FString& Filter, const FString& Category) const;

private:
	mutable FCriticalSection BufferLock;
	TArray<FString> Lines;      // ring storage, pre-sized to MaxLines
	TArray<FString> Categories; // parallel ring of category names
	int32 Head = 0;             // next write index
	int32 Num = 0;              // valid entries
};
