#include "Server/McpProtocol.h"

#include "AgentMcpSettings.h"
#include "Core/AgentMcpAuditLog.h"
#include "Core/AgentMcpTier.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealAgentMCPModule.h"

namespace
{
	const TCHAR* JsonRpcVersion = TEXT("2.0");

	FString MakeArgsDigest(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return TEXT("{}");
		}
		FString Digest;
		// Condensed (single-line) policy: the digest lands inside JSONL audit entries.
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Digest);
		FJsonSerializer::Serialize(Args.ToSharedRef(), Writer);
		constexpr int32 MaxDigestLen = 512;
		if (Digest.Len() > MaxDigestLen)
		{
			Digest.LeftInline(MaxDigestLen);
			Digest += TEXT("...");
		}
		return Digest;
	}

	const TCHAR* LatestProtocolVersion = TEXT("2025-06-18");
	const TCHAR* SupportedProtocolVersions[] = { TEXT("2025-06-18"), TEXT("2025-03-26") };

	constexpr int32 ParseErrorCode = -32700;
	constexpr int32 InvalidRequestCode = -32600;
	constexpr int32 MethodNotFoundCode = -32601;
	constexpr int32 InvalidParamsCode = -32602;

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString MakeErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), JsonRpcVersion);
		Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		Response->SetObjectField(TEXT("error"), Error);
		return SerializeObject(Response);
	}

	FString MakeResultResponse(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), JsonRpcVersion);
		Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		Response->SetObjectField(TEXT("result"), Result);
		return SerializeObject(Response);
	}

	/** Builds a tools/call result response from an FAgentMcpToolResult — shared by the normal-execute
	 *  path and the tier-rejection path. bRejectedByTier adds a structured discriminator field so
	 *  agents can branch on policy rejections without parsing message text (extra result fields are
	 *  legal per the MCP tools/call result schema). */
	FString MakeToolResultResponse(const TSharedPtr<FJsonValue>& Id, const FAgentMcpToolResult& ToolResult, bool bRejectedByTier = false)
	{
		TSharedRef<FJsonObject> ContentItem = MakeShared<FJsonObject>();
		ContentItem->SetStringField(TEXT("type"), TEXT("text"));
		ContentItem->SetStringField(TEXT("text"), ToolResult.Text);

		TArray<TSharedPtr<FJsonValue>> Content;
		Content.Add(MakeShared<FJsonValueObject>(ContentItem));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("content"), Content);
		Result->SetBoolField(TEXT("isError"), ToolResult.bIsError);
		if (bRejectedByTier)
		{
			Result->SetBoolField(TEXT("rejected_by_tier"), true);
		}
		return MakeResultResponse(Id, Result);
	}

	TSharedRef<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params)
	{
		FString ClientVersion;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), ClientVersion);
		}

		FString Negotiated = LatestProtocolVersion;
		for (const TCHAR* Supported : SupportedProtocolVersions)
		{
			if (ClientVersion == Supported)
			{
				Negotiated = ClientVersion;
				break;
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Negotiated);

		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("capabilities"), Capabilities);

		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("UnrealAgentMCP"));
		ServerInfo->SetStringField(TEXT("version"), AgentMcp::PluginVersion);
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		return Result;
	}

	TSharedRef<FJsonObject> HandleToolsList()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), FAgentMcpToolRegistry::Get().BuildToolsJson());
		return Result;
	}

	// Returns response string directly because unknown-tool is a protocol error (-32602 per MCP tools/call spec), not a tool result.
	FString HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return MakeErrorResponse(Id, InvalidParamsCode, TEXT("tools/call requires params with a 'name' field"));
		}
		FString ToolName;
		if (!Params->TryGetStringField(TEXT("name"), ToolName))
		{
			return MakeErrorResponse(Id, InvalidParamsCode, TEXT("tools/call params missing string field 'name'"));
		}
		const FAgentMcpToolDef* Tool = FAgentMcpToolRegistry::Get().Find(ToolName);
		if (!Tool)
		{
			return MakeErrorResponse(Id, InvalidParamsCode,
				FString::Printf(TEXT("Unknown tool '%s'. Call tools/list for available tools."), *ToolName));
		}

		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		Params->TryGetObjectField(TEXT("arguments"), ArgsPtr);
		const TSharedPtr<FJsonObject> Args = ArgsPtr ? *ArgsPtr : nullptr;

		// Single enforcement seam: every tool call passes through here (P2 final review). If a second
		// execution path is ever added (batch endpoint, resources), route it through this gate — the
		// check is bound to this handler, not to Handler.Execute itself.
		const UAgentMcpSettings* Settings = GetDefault<UAgentMcpSettings>();
		if (Tool->Tier > Settings->PermissionTier)
		{
			FAgentMcpAuditEntry Rejection;
			Rejection.Tool = ToolName;
			Rejection.ArgsDigest = MakeArgsDigest(Args);
			Rejection.bIsError = true;
			Rejection.bRejectedByTier = true;
			FAgentMcpAuditLog::Get().Append(Rejection);
			UE_LOG(LogAgentMcp, Warning, TEXT("tools/call %s rejected: tier %s exceeds ceiling %s"),
				*ToolName, AgentMcp::TierToString(Tool->Tier), AgentMcp::TierToString(Settings->PermissionTier));

			const FAgentMcpToolResult Rejected = FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Tool '%s' requires permission tier '%s' but the server ceiling is '%s'. If intended, raise it under Project Settings > Plugins > Unreal Agent MCP."),
				*ToolName, AgentMcp::TierToString(Tool->Tier), AgentMcp::TierToString(Settings->PermissionTier)));
			return MakeToolResultResponse(Id, Rejected, /*bRejectedByTier=*/true);
		}

		const double StartSeconds = FPlatformTime::Seconds();
		const FAgentMcpToolResult ToolResult = Tool->Handler.Execute(Args);
		const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		UE_LOG(LogAgentMcp, Display, TEXT("tools/call %s -> %s (%.1f ms)"),
			*ToolName, ToolResult.bIsError ? TEXT("error") : TEXT("ok"), ElapsedMs);

		FAgentMcpAuditEntry Audit;
		Audit.Tool = ToolName;
		Audit.ArgsDigest = MakeArgsDigest(Args);
		Audit.bIsError = ToolResult.bIsError;
		Audit.DurationMs = ElapsedMs;
		FAgentMcpAuditLog::Get().Append(Audit);

		return MakeToolResultResponse(Id, ToolResult);
	}
}

FString AgentMcp::Protocol::HandleMessage(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return MakeErrorResponse(nullptr, ParseErrorCode, TEXT("Request body is not valid JSON"));
	}

	// id is echoed verbatim without JSON-type validation (lenient for P1). Note: explicit "id":null parses
	// to a valid FJsonValueNull => request; only an ABSENT id field makes the message a notification.
	const TSharedPtr<FJsonValue> Id = Root->TryGetField(TEXT("id"));
	const bool bIsNotification = !Id.IsValid();

	FString Method;
	FString JsonRpc;
	Root->TryGetStringField(TEXT("jsonrpc"), JsonRpc);
	if (!Root->TryGetStringField(TEXT("method"), Method) || JsonRpc != JsonRpcVersion)
	{
		// Malformed notifications are dropped silently per JSON-RPC 2.0.
		return bIsNotification ? FString() : MakeErrorResponse(Id, InvalidRequestCode,
			TEXT("Request must have jsonrpc:\"2.0\" and a string 'method'"));
	}

	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	Root->TryGetObjectField(TEXT("params"), ParamsPtr);
	const TSharedPtr<FJsonObject> Params = ParamsPtr ? *ParamsPtr : nullptr;

	if (bIsNotification)
	{
		// P1 has no notification with side effects; acknowledge by silence.
		return FString();
	}

	if (Method == TEXT("initialize"))
	{
		return MakeResultResponse(Id, HandleInitialize(Params));
	}
	if (Method == TEXT("ping"))
	{
		return MakeResultResponse(Id, MakeShared<FJsonObject>());
	}
	if (Method == TEXT("tools/list"))
	{
		return MakeResultResponse(Id, HandleToolsList());
	}
	if (Method == TEXT("tools/call"))
	{
		return HandleToolsCall(Id, Params);
	}

	return MakeErrorResponse(Id, MethodNotFoundCode,
		FString::Printf(TEXT("Method '%s' is not supported"), *Method));
}
