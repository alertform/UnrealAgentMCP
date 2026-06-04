#include "UnrealAgentMCPModule.h"

#include "AgentMcpSettings.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Misc/CoreMisc.h"
#include "Server/McpHttpServer.h"
#include "Tools/AssetQueryTools.h"
#include "Tools/EditorInfoTools.h"

DEFINE_LOG_CATEGORY(LogAgentMcp);

FUnrealAgentMCPModule::~FUnrealAgentMCPModule() = default;

void FUnrealAgentMCPModule::StartupModule()
{
	AgentMcp::Tools::RegisterEditorInfoTools();
	AgentMcp::Tools::RegisterAssetQueryTools();
	UE_LOG(LogAgentMcp, Display, TEXT("UnrealAgentMCP: registered %d tools"), FAgentMcpToolRegistry::Get().Num());

	const UAgentMcpSettings* Settings = GetDefault<UAgentMcpSettings>();
	if (GIsEditor && !IsRunningCommandlet() && Settings->bAutoStartServer)
	{
		Server = MakeUnique<FMcpHttpServer>();
		if (!Server->Start(static_cast<uint32>(Settings->Port)))
		{
			Server.Reset();
		}
	}
}

void FUnrealAgentMCPModule::ShutdownModule()
{
	if (Server)
	{
		Server->Stop();
		Server.Reset();
	}
}

IMPLEMENT_MODULE(FUnrealAgentMCPModule, UnrealAgentMCP)
