#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMcpHttpServer;

DECLARE_LOG_CATEGORY_EXTERN(LogAgentMcp, Log, All);

class FUnrealAgentMCPModule : public IModuleInterface
{
public:
	/** Defined in the .cpp where FMcpHttpServer is a complete type (TUniquePtr pimpl). */
	virtual ~FUnrealAgentMCPModule() override;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TUniquePtr<FMcpHttpServer> Server;
};
