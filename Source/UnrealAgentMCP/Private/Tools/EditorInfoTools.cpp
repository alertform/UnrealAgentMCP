#include "Tools/EditorInfoTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FAgentMcpToolResult HandleEngineInfo(const TSharedPtr<FJsonObject>& /*Args*/)
	{
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Info->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Info->SetStringField(TEXT("plugin_version"), TEXT("0.1.0"));
		Info->SetBoolField(TEXT("is_editor"), GIsEditor);
		return FAgentMcpToolResult::Success(SerializeObject(Info));
	}
}

void AgentMcp::Tools::RegisterEditorInfoTools()
{
	FAgentMcpToolDef Def;
	Def.Name = TEXT("engine_info");
	Def.Description = TEXT("Returns Unreal Engine version, project name and plugin version. Use to verify the connection works.");
	Def.InputSchema = MakeShared<FJsonObject>();
	Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
	Def.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	Def.Tier = EAgentMcpTier::ReadOnly;
	Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleEngineInfo);
	FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
}
