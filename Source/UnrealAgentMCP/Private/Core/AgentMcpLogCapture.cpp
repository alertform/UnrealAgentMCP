#include "Core/AgentMcpLogCapture.h"

#include "Algo/Reverse.h"

FAgentMcpLogCapture::FAgentMcpLogCapture()
{
	// Pre-size in the constructor so Serialize never allocates the ring storage under the lock.
	Lines.SetNum(MaxLines);
	Categories.SetNum(MaxLines);
}

FAgentMcpLogCapture& FAgentMcpLogCapture::Get()
{
	static FAgentMcpLogCapture Instance;
	return Instance;
}

void FAgentMcpLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// NOTE: never log from inside Serialize — BufferLock is a non-recursive FCriticalSection and a
	// re-entrant log line would deadlock. This lock sits on the engine-wide logging hot path;
	// TODO(P4): consider a lock-free ring or double-buffer if shader-compile bursts become measurable.
	FScopeLock Lock(&BufferLock);
	Lines[Head] = FString::Printf(TEXT("[%s][%s] %s"), *Category.ToString(), ToString(Verbosity), V);
	Categories[Head] = Category.ToString();
	Head = (Head + 1) % MaxLines;
	Num = FMath::Min(Num + 1, MaxLines);
}

TArray<FString> FAgentMcpLogCapture::Recent(int32 Count, const FString& Filter, const FString& Category) const
{
	TArray<FString> Result;
	FScopeLock Lock(&BufferLock);
	// Walk newest -> oldest, collect matches, then reverse to chronological order.
	for (int32 Step = 0; Step < Num && Result.Num() < Count; ++Step)
	{
		const int32 Index = (Head - 1 - Step + MaxLines * 2) % MaxLines;
		if (!Category.IsEmpty() && Categories[Index] != Category)
		{
			continue;
		}
		if (!Filter.IsEmpty() && !Lines[Index].Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Result.Add(Lines[Index]);
	}
	Algo::Reverse(Result);
	return Result;
}
