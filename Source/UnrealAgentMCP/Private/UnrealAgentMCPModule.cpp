#include "UnrealAgentMCPModule.h"

#include "AgentMcpSettings.h"
#include "Core/AgentMcpLogCapture.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Misc/CoreMisc.h"
#include "Server/McpHttpServer.h"
#include "Tools/ActorTools.h"
#include "Tools/AssetImportTools.h"
#include "Tools/AssetQueryTools.h"
#include "Tools/EditorInfoTools.h"
#include "Tools/BlueprintTools.h"
#include "Tools/EditorSessionTools.h"
#include "Tools/NodeGraphTools.h"
#include "Tools/InputTools.h"
#include "Tools/VariableComponentTools.h"
#include "Tools/AnimMontageTools.h"
#include "Tools/BehaviorTreeTools.h"
#include "Tools/GameplayEffectTools.h"
#include "Tools/MvvmTools.h"
#include "Tools/WidgetTools.h"
#include "Tools/AnimGraphTools.h"
#include "Tools/RetargetTools.h"
#include "Tools/MaterialTools.h"

DEFINE_LOG_CATEGORY(LogAgentMcp);

FUnrealAgentMCPModule::~FUnrealAgentMCPModule() = default;

void FUnrealAgentMCPModule::StartupModule()
{
	AgentMcp::Tools::RegisterEditorInfoTools();
	AgentMcp::Tools::RegisterAssetQueryTools();
	AgentMcp::Tools::RegisterAssetImportTools();
	AgentMcp::Tools::RegisterNodeGraphTools();
	AgentMcp::Tools::RegisterGraphPinAndLayoutTools();
	AgentMcp::Tools::RegisterBlueprintTools();
	AgentMcp::Tools::RegisterEditorSessionTools();
	AgentMcp::Tools::RegisterActorTools();
	AgentMcp::Tools::RegisterInputTools();
	AgentMcp::Tools::RegisterVariableComponentTools();
	AgentMcp::Tools::RegisterWidgetTools();
	AgentMcp::Tools::RegisterWidgetEditTools();
	AgentMcp::Tools::RegisterMvvmTools();
	AgentMcp::Tools::RegisterMvvmBindingTools();
	AgentMcp::Tools::RegisterAnimMontageTools();
	AgentMcp::Tools::RegisterBehaviorTreeTools();
	AgentMcp::Tools::RegisterGameplayEffectTools();
	AgentMcp::Tools::RegisterAnimGraphTools();
	AgentMcp::Tools::RegisterRetargetTools();
	AgentMcp::Tools::RegisterMaterialTools();
	UE_LOG(LogAgentMcp, Display, TEXT("UnrealAgentMCP: registered %d tools"), FAgentMcpToolRegistry::Get().Num());
	GLog->AddOutputDevice(&FAgentMcpLogCapture::Get());

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
	if (GLog)
	{
		GLog->RemoveOutputDevice(&FAgentMcpLogCapture::Get());
	}
	if (Server)
	{
		Server->Stop();
		Server.Reset();
	}
}

IMPLEMENT_MODULE(FUnrealAgentMCPModule, UnrealAgentMCP)
