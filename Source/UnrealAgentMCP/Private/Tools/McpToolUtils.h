#pragma once

#include "CoreMinimal.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace AgentMcp::ToolUtils
{
	/** Serializes a JSON object to a compact string. Shared by tool implementations. */
	inline FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	// Schema sugar shared by tool families (namespace-qualified here rather than duplicated
	// in per-file anonymous namespaces — same-named anonymous symbols collide under the full
	// unity build once files leave the git working set; see the P6 C2084 lesson).

	/** One {type, description} schema property object. */
	inline TSharedRef<FJsonObject> TypedProp(const TCHAR* Type, const FString& Desc)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	}

	/** Builds an {type:"object", properties:{...}, required:[...]} input schema. */
	inline TSharedPtr<FJsonObject> MakeSchema(
		const TArray<TPair<FString, TSharedRef<FJsonObject>>>& Props, const TArray<FString>& Required)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Props) { P->SetObjectField(Pair.Key, Pair.Value); }
		Schema->SetObjectField(TEXT("properties"), P);
		TArray<TSharedPtr<FJsonValue>> Req;
		for (const FString& R : Required) { Req.Add(MakeShared<FJsonValueString>(R)); }
		Schema->SetArrayField(TEXT("required"), Req);
		return Schema;
	}

	/** Registers one SafeWrite tool with a static handler in a single call. */
	inline void RegisterOne(const TCHAR* Name, const FString& Desc,
		const TArray<TPair<FString, TSharedRef<FJsonObject>>>& Props, const TArray<FString>& Required,
		FAgentMcpToolResult (*Fn)(const TSharedPtr<FJsonObject>&))
	{
		FAgentMcpToolDef Def;
		Def.Name = Name;
		Def.Description = Desc;
		Def.InputSchema = MakeSchema(Props, Required);
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(Fn);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
