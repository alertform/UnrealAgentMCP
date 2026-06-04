#include "Tools/NodeGraphUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	FString DescribeAvailableGraphs(const UBlueprint* Blueprint)
	{
		// MacroGraphs/DelegateSignatureGraphs are intentionally omitted: they are not node-editing targets in P2.
		TArray<FString> Names;
		for (const TObjectPtr<UEdGraph>& Graph : Blueprint->UbergraphPages)
		{
			Names.Add(Graph->GetName());
		}
		for (const TObjectPtr<UEdGraph>& Graph : Blueprint->FunctionGraphs)
		{
			Names.Add(Graph->GetName());
		}
		return FString::Join(Names, TEXT(", "));
	}
}

UBlueprint* AgentMcp::NodeGraphUtils::ResolveBlueprint(const FString& Path, FString& OutError)
{
	if (Path.StartsWith(TEXT("/Script/")))
	{
		OutError = FString::Printf(TEXT("'%s' is a native class path, not a Blueprint asset path. Use a /Game/... path."), *Path);
		return nullptr;
	}
	FString ObjectPath = Path;
	ObjectPath.RemoveFromEnd(TEXT("/"));
	if (!ObjectPath.Contains(TEXT(".")))
	{
		FString Leaf;
		if (!ObjectPath.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			Leaf = ObjectPath;
		}
		ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *Leaf);
	}

	UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *ObjectPath);
	if (!Blueprint)
	{
		Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	}
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: '%s'. Use list_assets to discover paths."), *Path);
	}
	return Blueprint;
}

UEdGraph* AgentMcp::NodeGraphUtils::ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
{
	// "EventGraph" is a sentinel alias for UbergraphPages[0] regardless of the page's actual name,
	// matching the editor convention that the primary event graph is always first.
	if (GraphName.IsEmpty() || GraphName == TEXT("EventGraph"))
	{
		if (Blueprint->UbergraphPages.Num() > 0)
		{
			return Blueprint->UbergraphPages[0];
		}
		OutError = TEXT("Blueprint has no event graph.");
		return nullptr;
	}
	for (const TObjectPtr<UEdGraph>& Graph : Blueprint->UbergraphPages)
	{
		if (Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}
	for (const TObjectPtr<UEdGraph>& Graph : Blueprint->FunctionGraphs)
	{
		if (Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}
	OutError = FString::Printf(TEXT("Graph '%s' not found. Available graphs: %s"), *GraphName, *DescribeAvailableGraphs(Blueprint));
	return nullptr;
}

UEdGraphNode* AgentMcp::NodeGraphUtils::FindNodeByGuid(UEdGraph* Graph, const FString& GuidString, FString& OutError)
{
	FGuid Guid;
	if (!FGuid::Parse(GuidString, Guid))
	{
		OutError = FString::Printf(TEXT("'%s' is not a valid node id (expected a GUID from read_graph/add_node)."), *GuidString);
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}
	OutError = FString::Printf(TEXT("No node with id '%s' in graph '%s'. Call read_graph for current ids."), *GuidString, *Graph->GetName());
	return nullptr;
}

UEdGraphPin* AgentMcp::NodeGraphUtils::FindPin(UEdGraphNode* Node, const FString& PinName, int32 PreferredDirection, FString& OutError)
{
	UEdGraphPin* Fallback = nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString() == PinName)
		{
			if (PreferredDirection == EGPD_MAX || Pin->Direction == PreferredDirection)
			{
				return Pin;
			}
			Fallback = Pin;
		}
	}
	if (Fallback)
	{
		// Direction mismatch: the only pin with this name has the opposite direction. Return it so
		// read paths still work, but surface the mismatch for write-path callers/diagnostics.
		OutError = FString::Printf(TEXT("Pin '%s' exists but its direction does not match the preferred direction; verify with read_graph."), *PinName);
		return Fallback;
	}
	TArray<FString> Names;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Names.Add(Pin->PinName.ToString());
		}
	}
	OutError = FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available pins: %s"),
		*PinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *FString::Join(Names, TEXT(", ")));
	return nullptr;
}

TSharedRef<FJsonObject> AgentMcp::NodeGraphUtils::PinToJson(const UEdGraphPin* Pin)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Pin->PinId.ToString());
	Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
	Json->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
	Json->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	if (!Pin->DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default_value"), Pin->DefaultValue);
	}
	TArray<TSharedPtr<FJsonValue>> Links;
	for (const UEdGraphPin* Linked : Pin->LinkedTo)
	{
		if (Linked && Linked->GetOwningNode())
		{
			TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("node_id"), Linked->GetOwningNode()->NodeGuid.ToString());
			Link->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(Link));
		}
	}
	Json->SetArrayField(TEXT("links"), Links);
	return Json;
}

TSharedRef<FJsonObject> AgentMcp::NodeGraphUtils::NodeToJson(const UEdGraphNode* Node)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
	Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Json->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());
	TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
	Pos->SetNumberField(TEXT("x"), Node->NodePosX);
	Pos->SetNumberField(TEXT("y"), Node->NodePosY);
	Json->SetObjectField(TEXT("pos"), Pos);
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
		}
	}
	Json->SetArrayField(TEXT("pins"), Pins);
	return Json;
}

TSharedRef<FJsonObject> AgentMcp::NodeGraphUtils::GraphToJson(const UBlueprint* Blueprint, const UEdGraph* Graph)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Json->SetStringField(TEXT("graph"), Graph->GetName());
	// TODO(P3): add a node-count cap (cf. list_assets limit) before this runs against
	// production-scale graphs; P2 test blueprints are intentionally small.
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
		}
	}
	Json->SetNumberField(TEXT("node_count"), Nodes.Num());
	Json->SetArrayField(TEXT("nodes"), Nodes);
	return Json;
}
