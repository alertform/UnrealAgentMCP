#include "Core/AgentMcpAuditLog.h"

#include "AgentMcpSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FAgentMcpAuditLog& FAgentMcpAuditLog::Get()
{
	static FAgentMcpAuditLog Instance;
	return Instance;
}

FString FAgentMcpAuditLog::CurrentFilePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentMCP"),
		FString::Printf(TEXT("audit-%s.jsonl"), *FDateTime::Now().ToString(TEXT("%Y%m%d"))));
}

void FAgentMcpAuditLog::Append(const FAgentMcpAuditEntry& Entry)
{
	if (!GetDefault<UAgentMcpSettings>()->bEnableAuditLog)
	{
		return;
	}

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("ts"), FDateTime::Now().ToIso8601());
	Json->SetStringField(TEXT("tool"), Entry.Tool);
	Json->SetStringField(TEXT("args"), Entry.ArgsDigest);
	Json->SetBoolField(TEXT("is_error"), Entry.bIsError);
	Json->SetBoolField(TEXT("rejected_by_tier"), Entry.bRejectedByTier);
	Json->SetNumberField(TEXT("duration_ms"), Entry.DurationMs);

	// Use condensed (single-line) writer — JSONL requires one JSON object per line.
	FString Line;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
	FJsonSerializer::Serialize(Json, Writer);
	Line += LINE_TERMINATOR;

	FScopeLock Lock(&FileLock);
	const FString FilePath = CurrentFilePath();
	// Ensure the directory exists before appending (FFileHelper::SaveStringToFile does NOT create dirs).
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);
	FFileHelper::SaveStringToFile(Line, *FilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

TArray<FString> FAgentMcpAuditLog::Tail(int32 Count) const
{
	TArray<FString> AllLines;
	{
		FScopeLock Lock(&FileLock);
		FFileHelper::LoadFileToStringArray(AllLines, *CurrentFilePath());
	}
	if (AllLines.Num() > Count)
	{
		AllLines.RemoveAt(0, AllLines.Num() - Count);
	}
	return AllLines;
}
