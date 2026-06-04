#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Core/AgentMcpTier.h"
#include "AgentMcpSettings.generated.h"

/** Project Settings > Plugins > Unreal Agent MCP */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Unreal Agent MCP"))
class UAgentMcpSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** TCP port for the MCP HTTP endpoint. Bound to loopback only. Change requires editor restart. */
	UPROPERTY(EditAnywhere, config, Category = "Server", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 Port = 17777;

	/** Start the MCP server automatically when the editor launches. */
	UPROPERTY(EditAnywhere, config, Category = "Server")
	bool bAutoStartServer = true;

	/** Maximum permission tier agents may use. Tools above this tier are rejected. Enforced from P3. */
	UPROPERTY(EditAnywhere, config, Category = "Safety")
	EAgentMcpTier PermissionTier = EAgentMcpTier::SafeWrite;
};
