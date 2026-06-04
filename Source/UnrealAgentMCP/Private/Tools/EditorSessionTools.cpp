#include "Tools/EditorSessionTools.h"

#include "Core/AgentMcpLogCapture.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/McpToolUtils.h"

namespace
{
	using namespace AgentMcp;

	FAgentMcpToolResult HandleReadOutputLog(const TSharedPtr<FJsonObject>& Args)
	{
		int32 Count = 100;
		FString Filter, Category;
		if (Args.IsValid())
		{
			double Number = 0.0;
			if (Args->TryGetNumberField(TEXT("lines"), Number))
			{
				Count = FMath::Clamp(static_cast<int32>(Number), 1, FAgentMcpLogCapture::MaxLines);
			}
			Args->TryGetStringField(TEXT("filter"), Filter);
			Args->TryGetStringField(TEXT("category"), Category);
		}
		const TArray<FString> Recent = FAgentMcpLogCapture::Get().Recent(Count, Filter, Category);
		TArray<TSharedPtr<FJsonValue>> LineValues;
		LineValues.Reserve(Recent.Num());
		for (const FString& Line : Recent)
		{
			LineValues.Add(MakeShared<FJsonValueString>(Line));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("returned"), Recent.Num());
		Result->SetArrayField(TEXT("lines"), LineValues);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}
}

void AgentMcp::Tools::RegisterEditorSessionTools()
{
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("read_output_log");
		Def.Description = TEXT("Returns recent editor Output Log lines from an in-memory ring buffer (last 2000 lines since editor start). Args: lines (int, default 100), filter (case-insensitive substring), category (exact log category, e.g. LogBlueprint). Use after compile_blueprint or console_command to inspect engine output.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> LinesProp = MakeShared<FJsonObject>();
			LinesProp->SetStringField(TEXT("type"), TEXT("integer"));
			LinesProp->SetStringField(TEXT("description"), TEXT("Max lines to return (1-2000, default 100)"));
			Properties->SetObjectField(TEXT("lines"), LinesProp);
			TSharedRef<FJsonObject> FilterProp = MakeShared<FJsonObject>();
			FilterProp->SetStringField(TEXT("type"), TEXT("string"));
			FilterProp->SetStringField(TEXT("description"), TEXT("Case-insensitive substring filter"));
			Properties->SetObjectField(TEXT("filter"), FilterProp);
			TSharedRef<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
			CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
			CategoryProp->SetStringField(TEXT("description"), TEXT("Exact log category name, e.g. LogAgentMcp"));
			Properties->SetObjectField(TEXT("category"), CategoryProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleReadOutputLog);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
