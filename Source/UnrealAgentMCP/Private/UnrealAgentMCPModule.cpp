#include "UnrealAgentMCPModule.h"

DEFINE_LOG_CATEGORY(LogAgentMcp);

void FUnrealAgentMCPModule::StartupModule()
{
	UE_LOG(LogAgentMcp, Display, TEXT("UnrealAgentMCP module starting (P1 skeleton)"));
}

void FUnrealAgentMCPModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FUnrealAgentMCPModule, UnrealAgentMCP)
