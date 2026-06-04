#include "Tools/NodeGraphTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"

namespace
{
	using namespace AgentMcp;

	/** Shared arg unpacking for every node-graph tool: blueprint_path (required) + graph_name (optional). */
	bool ResolveGraphArgs(const TSharedPtr<FJsonObject>& Args, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, FString& OutError)
	{
		if (!Args.IsValid())
		{
			OutError = TEXT("Missing arguments: blueprint_path is required.");
			return false;
		}
		FString BlueprintPath;
		if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			OutError = TEXT("Missing required string argument 'blueprint_path'.");
			return false;
		}
		OutBlueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, OutError);
		if (!OutBlueprint)
		{
			return false;
		}
		FString GraphName;
		Args->TryGetStringField(TEXT("graph_name"), GraphName);
		OutGraph = NodeGraphUtils::ResolveGraph(OutBlueprint, GraphName, OutError);
		return OutGraph != nullptr;
	}

	FAgentMcpToolResult HandleReadGraph(const TSharedPtr<FJsonObject>& Args)
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		FString Error;
		if (!ResolveGraphArgs(Args, Blueprint, Graph, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(NodeGraphUtils::GraphToJson(Blueprint, Graph)));
	}

	TSharedRef<FJsonObject> MakeGraphArgsSchema()
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"), TEXT("Blueprint asset path, e.g. /Game/Blueprints/BP_Foo"));
		Properties->SetObjectField(TEXT("blueprint_path"), PathProp);
		TSharedRef<FJsonObject> GraphProp = MakeShared<FJsonObject>();
		GraphProp->SetStringField(TEXT("type"), TEXT("string"));
		GraphProp->SetStringField(TEXT("description"), TEXT("Graph name; defaults to the event graph"));
		Properties->SetObjectField(TEXT("graph_name"), GraphProp);
		Schema->SetObjectField(TEXT("properties"), Properties);
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
		Schema->SetArrayField(TEXT("required"), Required);
		return Schema;
	}
}

void AgentMcp::Tools::RegisterNodeGraphTools()
{
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("read_graph");
		Def.Description = TEXT("Reads a Blueprint graph as JSON: nodes with ids, classes, titles, pins (name/direction/type/default) and links. Always call this before and after editing to see real graph state. Args: blueprint_path (required), graph_name (default EventGraph).");
		Def.InputSchema = MakeGraphArgsSchema();
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleReadGraph);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
