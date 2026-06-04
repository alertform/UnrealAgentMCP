#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class FJsonObject;
class FJsonValue;

namespace AgentMcp::NodeGraphUtils
{
	/** FindObject first (transient/loaded), then LoadObject. Accepts /Game/X/BP_Y and /Game/X/BP_Y.BP_Y forms. nullptr + OutError on failure. */
	UBlueprint* ResolveBlueprint(const FString& Path, FString& OutError);

	/** Empty/"EventGraph" -> first ubergraph page; otherwise matches UbergraphPages + FunctionGraphs by name. nullptr + OutError (listing available graph names) on failure. */
	UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError);

	/** Finds a node by NodeGuid string (FGuid::Parse). nullptr + OutError on bad guid or no match. */
	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString, FString& OutError);

	/** Finds a pin by name. PreferredDirection breaks ties when both an input and output share the name; pass EGPD_MAX for no preference. nullptr + OutError (listing the node's pin names) on failure. */
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, int32 PreferredDirection, FString& OutError);

	/** {id, name, direction, type, default_value, links:[{node_id, pin_name}]} */
	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin);

	/** {id, class, title, enabled, pos:{x,y}, pins:[...]} */
	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node);

	/** {blueprint, graph, node_count, nodes:[...]} */
	TSharedRef<FJsonObject> GraphToJson(const UBlueprint* Blueprint, const UEdGraph* Graph);
}
