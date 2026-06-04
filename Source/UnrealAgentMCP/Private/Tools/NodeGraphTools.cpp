#include "Tools/NodeGraphTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "UObject/UObjectGlobals.h"

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

	template <typename TNodeClass>
	TNodeClass* SpawnSimpleNode(UEdGraph* Graph, int32 PosX, int32 PosY)
	{
		FGraphNodeCreator<TNodeClass> Creator(*Graph);
		TNodeClass* Node = Creator.CreateNode();
		Node->NodePosX = PosX;
		Node->NodePosY = PosY;
		Creator.Finalize();
		return Node;
	}

	FAgentMcpToolResult MakeNodeResult(UEdGraphNode* Node, bool bExisting)
	{
		TSharedRef<FJsonObject> Result = NodeGraphUtils::NodeToJson(Node);
		// NodeToJson uses "id"; tools/call consumers expect "node_id" as the primary handle — provide both.
		Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
		Result->SetBoolField(TEXT("existing"), bExisting);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	/** Converts a ghost (disabled) auto-placed event node into a fully enabled node. */
	void EnableGhostNode(UEdGraphNode* Node)
	{
		Node->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/false);
		// Clear the ghost comment ("This node is disabled and will not be called...") so the node
		// renders as a normal enabled node. bUserSetEnabledState stays false by design — the agent,
		// not the user, is activating it.
		if (Node->NodeComment.StartsWith(TEXT("This node is disabled")))
		{
			Node->NodeComment.Empty();
		}
	}

	FAgentMcpToolResult HandleAddNode(const TSharedPtr<FJsonObject>& Args)
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		FString Error;
		if (!ResolveGraphArgs(Args, Blueprint, Graph, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}
		FString NodeType;
		if (!Args->TryGetStringField(TEXT("node_type"), NodeType))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'node_type' (event|call_function|branch|sequence|variable_get|variable_set|self)."));
		}
		int32 PosX = 0, PosY = 0;
		double Number = 0.0;
		if (Args->TryGetNumberField(TEXT("pos_x"), Number)) { PosX = static_cast<int32>(Number); }
		if (Args->TryGetNumberField(TEXT("pos_y"), Number)) { PosY = static_cast<int32>(Number); }

		const FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddNode", "MCP: Add Node"));
		Graph->Modify();

		if (NodeType == TEXT("event"))
		{
			FString EventName;
			if (!Args->TryGetStringField(TEXT("event_name"), EventName))
			{
				return FAgentMcpToolResult::Error(TEXT("node_type 'event' requires 'event_name' (UFunction name, e.g. ReceiveBeginPlay, ReceiveTick)."));
			}
			// Reuse an existing event node (including disabled ghost defaults) instead of duplicating.
			for (UEdGraphNode* Existing : Graph->Nodes)
			{
				if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Existing))
				{
					if (EventNode->EventReference.GetMemberName() == FName(*EventName))
					{
						if (!EventNode->IsNodeEnabled())
						{
							EventNode->Modify();
							EnableGhostNode(EventNode);
							// Enabling a ghost is structural: the node just entered the compile graph.
							FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
						}
						return MakeNodeResult(EventNode, /*bExisting=*/true);
					}
				}
			}
			// No existing node found — create a new one via AddDefaultEventNode, then enable it
			// (AddDefaultEventNode always calls MakeAutomaticallyPlacedGhostNode at the end).
			UClass* EventClass = Blueprint->ParentClass;
			int32 NodePosY = PosY;
			UK2Node_Event* NewEvent = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, FName(*EventName), EventClass, NodePosY);
			if (!NewEvent)
			{
				return FAgentMcpToolResult::Error(FString::Printf(TEXT("Event '%s' not found on parent class '%s'."), *EventName, *GetNameSafe(EventClass)));
			}
			NewEvent->Modify();
			NewEvent->NodePosX = PosX;
			// AddDefaultEventNode always makes the node a ghost — activate it immediately.
			EnableGhostNode(NewEvent);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return MakeNodeResult(NewEvent, /*bExisting=*/false);
		}
		if (NodeType == TEXT("call_function"))
		{
			FString FunctionName;
			if (!Args->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				return FAgentMcpToolResult::Error(TEXT("node_type 'call_function' requires 'function_name'."));
			}
			FString ClassName;
			Args->TryGetStringField(TEXT("class_name"), ClassName);
			UClass* OwnerClass = nullptr;
			if (ClassName.IsEmpty())
			{
				OwnerClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
			}
			else
			{
				OwnerClass = UClass::TryFindTypeSlow<UClass>(ClassName);
				if (!OwnerClass)
				{
					return FAgentMcpToolResult::Error(FString::Printf(TEXT("Class '%s' not found. Use a short name like 'KismetSystemLibrary' or a /Script/ path."), *ClassName));
				}
			}
			UFunction* Function = OwnerClass->FindFunctionByName(FName(*FunctionName));
			if (!Function)
			{
				return FAgentMcpToolResult::Error(FString::Printf(TEXT("Function '%s' not found on class '%s'."), *FunctionName, *OwnerClass->GetName()));
			}
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->SetFromFunction(Function);
			Node->NodePosX = PosX;
			Node->NodePosY = PosY;
			Creator.Finalize();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return MakeNodeResult(Node, /*bExisting=*/false);
		}
		if (NodeType == TEXT("variable_get") || NodeType == TEXT("variable_set"))
		{
			FString VariableName;
			if (!Args->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				return FAgentMcpToolResult::Error(TEXT("variable_get/variable_set require 'variable_name'."));
			}
			UClass* OwnerClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
			const FProperty* Property = OwnerClass ? OwnerClass->FindPropertyByName(FName(*VariableName)) : nullptr;
			if (!Property)
			{
				return FAgentMcpToolResult::Error(FString::Printf(TEXT("Variable '%s' not found on '%s'."), *VariableName, *GetNameSafe(OwnerClass)));
			}
			UEdGraphNode* Node = nullptr;
			if (NodeType == TEXT("variable_get"))
			{
				FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
				UK2Node_VariableGet* GetNode = Creator.CreateNode();
				GetNode->VariableReference.SetSelfMember(FName(*VariableName));
				GetNode->NodePosX = PosX;
				GetNode->NodePosY = PosY;
				Creator.Finalize();
				Node = GetNode;
			}
			else
			{
				FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
				UK2Node_VariableSet* SetNode = Creator.CreateNode();
				SetNode->VariableReference.SetSelfMember(FName(*VariableName));
				SetNode->NodePosX = PosX;
				SetNode->NodePosY = PosY;
				Creator.Finalize();
				Node = SetNode;
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return MakeNodeResult(Node, /*bExisting=*/false);
		}
		if (NodeType == TEXT("branch"))
		{
			UEdGraphNode* Node = SpawnSimpleNode<UK2Node_IfThenElse>(Graph, PosX, PosY);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return MakeNodeResult(Node, false);
		}
		if (NodeType == TEXT("sequence"))
		{
			UEdGraphNode* Node = SpawnSimpleNode<UK2Node_ExecutionSequence>(Graph, PosX, PosY);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return MakeNodeResult(Node, false);
		}
		if (NodeType == TEXT("self"))
		{
			UEdGraphNode* Node = SpawnSimpleNode<UK2Node_Self>(Graph, PosX, PosY);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return MakeNodeResult(Node, false);
		}
		return FAgentMcpToolResult::Error(FString::Printf(TEXT("Unknown node_type '%s'. Supported: event, call_function, branch, sequence, variable_get, variable_set, self."), *NodeType));
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

	TSharedRef<FJsonObject> MakeAddNodeSchema()
	{
		// Start from the base graph args schema (blueprint_path required, graph_name optional), then
		// extend the properties object with add_node-specific parameters.
		TSharedRef<FJsonObject> Schema = MakeGraphArgsSchema();
		// Retrieve (or re-create) the properties object to add more fields.
		const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr)
		{
			// Fallback: rebuild properties from scratch.
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			Schema->SetObjectField(TEXT("properties"), Props);
			PropertiesPtr = nullptr;
		}
		// Work directly through Schema API since we can't hold a non-const ref across the TryGetObjectField call.
		{
			TSharedRef<FJsonObject> NodeTypeProp = MakeShared<FJsonObject>();
			NodeTypeProp->SetStringField(TEXT("type"), TEXT("string"));
			NodeTypeProp->SetStringField(TEXT("description"),
				TEXT("Type of node to add. One of: event, call_function, branch, sequence, variable_get, variable_set, self"));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("node_type"), NodeTypeProp);
		}
		{
			TSharedRef<FJsonObject> EventNameProp = MakeShared<FJsonObject>();
			EventNameProp->SetStringField(TEXT("type"), TEXT("string"));
			EventNameProp->SetStringField(TEXT("description"),
				TEXT("UFunction name of the event to add (node_type=event only), e.g. ReceiveBeginPlay, ReceiveTick"));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("event_name"), EventNameProp);
		}
		{
			TSharedRef<FJsonObject> ClassNameProp = MakeShared<FJsonObject>();
			ClassNameProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassNameProp->SetStringField(TEXT("description"),
				TEXT("Class that owns the function (node_type=call_function). Short name like 'KismetSystemLibrary' or /Script/ path. Omit to use the Blueprint's own class."));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("class_name"), ClassNameProp);
		}
		{
			TSharedRef<FJsonObject> FunctionNameProp = MakeShared<FJsonObject>();
			FunctionNameProp->SetStringField(TEXT("type"), TEXT("string"));
			FunctionNameProp->SetStringField(TEXT("description"),
				TEXT("Name of the function to call (node_type=call_function), e.g. PrintString"));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("function_name"), FunctionNameProp);
		}
		{
			TSharedRef<FJsonObject> VariableNameProp = MakeShared<FJsonObject>();
			VariableNameProp->SetStringField(TEXT("type"), TEXT("string"));
			VariableNameProp->SetStringField(TEXT("description"),
				TEXT("Variable name to get or set (node_type=variable_get or variable_set)"));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("variable_name"), VariableNameProp);
		}
		{
			TSharedRef<FJsonObject> PosXProp = MakeShared<FJsonObject>();
			PosXProp->SetStringField(TEXT("type"), TEXT("integer"));
			PosXProp->SetStringField(TEXT("description"), TEXT("Horizontal position of the new node in the graph (default 0)"));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("pos_x"), PosXProp);
		}
		{
			TSharedRef<FJsonObject> PosYProp = MakeShared<FJsonObject>();
			PosYProp->SetStringField(TEXT("type"), TEXT("integer"));
			PosYProp->SetStringField(TEXT("description"), TEXT("Vertical position of the new node in the graph (default 0)"));
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("pos_y"), PosYProp);
		}
		// Extend the required array with node_type.
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("node_type")));
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
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_node");
		Def.Description = TEXT("Adds a node to a Blueprint graph. node_type: event (event_name e.g. ReceiveBeginPlay; reuses existing/ghost events and enables them), call_function (class_name optional + function_name), branch, sequence, variable_get/variable_set (variable_name), self. Optional pos_x/pos_y. Returns the new node with node_id and pins - connect them with connect_pins, then compile_blueprint.");
		Def.InputSchema = MakeAddNodeSchema();
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddNode);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
