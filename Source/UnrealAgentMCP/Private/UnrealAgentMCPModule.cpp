#include "UnrealAgentMCPModule.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Tools/AssetQueryTools.h"
#include "Tools/EditorInfoTools.h"

DEFINE_LOG_CATEGORY(LogAgentMcp);

void FUnrealAgentMCPModule::StartupModule()
{
	AgentMcp::Tools::RegisterEditorInfoTools();
	AgentMcp::Tools::RegisterAssetQueryTools();
	UE_LOG(LogAgentMcp, Display, TEXT("UnrealAgentMCP: registered %d tools"), FAgentMcpToolRegistry::Get().Num());
}

void FUnrealAgentMCPModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FUnrealAgentMCPModule, UnrealAgentMCP)
