#include "Core/AgentMcpToolRegistry.h"

#include "Dom/JsonValue.h"
#include "UnrealAgentMCPModule.h"

FAgentMcpToolRegistry& FAgentMcpToolRegistry::Get()
{
	static FAgentMcpToolRegistry Instance;
	return Instance;
}

void FAgentMcpToolRegistry::Register(FAgentMcpToolDef&& Def)
{
	if (Def.Name.IsEmpty() || !Def.Handler.IsBound())
	{
		UE_LOG(LogAgentMcp, Warning, TEXT("Rejected invalid tool registration (empty name or unbound handler)"));
		return;
	}
	const FString Name = Def.Name;
	Tools.Add(Name, MoveTemp(Def));
}

const FAgentMcpToolDef* FAgentMcpToolRegistry::Find(const FString& Name) const
{
	return Tools.Find(Name);
}

TArray<TSharedPtr<FJsonValue>> FAgentMcpToolRegistry::BuildToolsJson() const
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Tools.Num());
	for (const TPair<FString, FAgentMcpToolDef>& Pair : Tools)
	{
		TSharedRef<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), Pair.Value.Name);
		ToolObj->SetStringField(TEXT("description"), Pair.Value.Description);

		TSharedPtr<FJsonObject> Schema;
		if (Pair.Value.InputSchema.IsValid())
		{
			Schema = Pair.Value.InputSchema;
		}
		else
		{
			Schema = MakeShared<FJsonObject>();
		}
		ToolObj->SetObjectField(TEXT("inputSchema"), Schema);

		Result.Add(MakeShared<FJsonValueObject>(ToolObj));
	}
	return Result;
}
