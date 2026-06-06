#include "Core/AgentMcpAuditLog.h"

#include "AgentMcpSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealAgentMCPModule.h"

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
	// Called on the game thread (dispatch seam); plain CDO bool read is safe here.
	if (!GetDefault<UAgentMcpSettings>()->bEnableAuditLog)
	{
		return;
	}

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("ts"), FDateTime::Now().ToIso8601());
	Json->SetStringField(TEXT("tool"), Entry.Tool);
	// "ArgsDigest" is a plain TRUNCATED JSON dump, NOT a cryptographic digest. Do not pass
	// secrets in tool args unless you accept them landing in this plaintext audit file.
	Json->SetStringField(TEXT("args"), Entry.ArgsDigest);
	Json->SetBoolField(TEXT("is_error"), Entry.bIsError);
	if (Entry.bIsError && !Entry.ErrorText.IsEmpty())
	{
		Json->SetStringField(TEXT("error"), Entry.ErrorText.Left(300));
	}
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
	if (!FFileHelper::SaveStringToFile(Line, *FilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
	{
		// Warn once: a silently incomplete audit trail is worse than a noisy one.
		static bool bWarnedOnce = false;
		if (!bWarnedOnce)
		{
			bWarnedOnce = true;
			UE_LOG(LogAgentMcp, Warning, TEXT("FAgentMcpAuditLog: failed to append to '%s' (disk full or file locked); further failures will not be re-logged"), *FilePath);
		}
	}
}

TArray<FString> FAgentMcpAuditLog::Tail(int32 Count) const
{
	TArray<FString> AllLines;
	{
		FScopeLock Lock(&FileLock);
		// TODO(P4): LoadFileToStringArray reads the whole file; at agent-paced call rates a full
		// day stays small (<50 MB), but a reverse-seek tail would be better for marathon sessions.
		FFileHelper::LoadFileToStringArray(AllLines, *CurrentFilePath());
	}
	if (AllLines.Num() > Count)
	{
		AllLines.RemoveAt(0, AllLines.Num() - Count);
	}
	return AllLines;
}
