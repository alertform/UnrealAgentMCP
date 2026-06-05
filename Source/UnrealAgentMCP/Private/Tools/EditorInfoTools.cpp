#include "Tools/EditorInfoTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Tools/McpToolUtils.h"

namespace
{
	FAgentMcpToolResult HandleEngineInfo(const TSharedPtr<FJsonObject>& /*Args*/)
	{
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Info->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Info->SetStringField(TEXT("plugin_version"), AgentMcp::PluginVersion);
		Info->SetBoolField(TEXT("is_editor"), GIsEditor);

		// Report the currently loaded map package name (empty string when no world is loaded).
		FString CurrentLevel;
		if (GEditor)
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (World)
			{
				CurrentLevel = World->GetOutermost()->GetName();
			}
		}
		Info->SetStringField(TEXT("current_level"), CurrentLevel);

		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Info));
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
