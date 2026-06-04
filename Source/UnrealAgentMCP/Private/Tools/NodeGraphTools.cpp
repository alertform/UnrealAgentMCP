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
		// Capture BEFORE SetEnabledState flips the state this check reads.
		const bool bWasAutoGhost = Node->IsAutomaticallyPlacedGhostNode();
		Node->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/false);
		// The ghost comment is engine-generated LOCALIZED text ("This node is disabled...", translated
		// per editor language) — never prefix-match it. Auto-ghost status is the reliable signal; the
		// comment is engine boilerplate there, safe to clear unconditionally. bUserSetEnabledState
		// stays false by design — the agent, not the user, is activating it.
		if (bWasAutoGhost)
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

		// Non-const so validation-error paths can Cancel() — otherwise every agent retry on a bad
		// function/variable name would leave an empty "MCP: Add Node" entry in the undo history.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddNode", "MCP: Add Node"));
		Graph->Modify();

		if (NodeType == TEXT("event"))
		{
			FString EventName;
			if (!Args->TryGetStringField(TEXT("event_name"), EventName))
			{
				Transaction.Cancel();
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
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(TEXT("Event '%s' is not available on parent class '%s' (not found, hidden, or disallowed)."), *EventName, *GetNameSafe(EventClass)));
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
				Transaction.Cancel();
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
					Transaction.Cancel();
					return FAgentMcpToolResult::Error(FString::Printf(TEXT("Class '%s' not found. Use a short name like 'KismetSystemLibrary' or a /Script/ path."), *ClassName));
				}
			}
			UFunction* Function = OwnerClass->FindFunctionByName(FName(*FunctionName));
			if (!Function)
			{
				Transaction.Cancel();
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
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(TEXT("variable_get/variable_set require 'variable_name'."));
			}
			UClass* OwnerClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->ParentClass.Get();
			const FProperty* Property = OwnerClass ? OwnerClass->FindPropertyByName(FName(*VariableName)) : nullptr;
			if (!Property)
			{
				Transaction.Cancel();
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
		Transaction.Cancel();
		return FAgentMcpToolResult::Error(FString::Printf(TEXT("Unknown node_type '%s'. Supported: event, call_function, branch, sequence, variable_get, variable_set, self."), *NodeType));
	}

	// -----------------------------------------------------------------------
	// connect_pins
	// -----------------------------------------------------------------------
	FAgentMcpToolResult HandleConnectPins(const TSharedPtr<FJsonObject>& Args)
	{
		// --- Validate all args before opening a transaction ---
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		FString Error;
		if (!ResolveGraphArgs(Args, Blueprint, Graph, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}
		FString FromNodeId, FromPinName, ToNodeId, ToPinName;
		if (!Args->TryGetStringField(TEXT("from_node_id"), FromNodeId) ||
			!Args->TryGetStringField(TEXT("from_pin"), FromPinName) ||
			!Args->TryGetStringField(TEXT("to_node_id"), ToNodeId) ||
			!Args->TryGetStringField(TEXT("to_pin"), ToPinName))
		{
			return FAgentMcpToolResult::Error(TEXT("connect_pins requires from_node_id, from_pin, to_node_id, to_pin."));
		}
		// Fix 4: self-connection early-out
		if (FromNodeId == ToNodeId)
		{
			return FAgentMcpToolResult::Error(TEXT("from_node_id and to_node_id are the same node; self-connections are not permitted."));
		}
		UEdGraphNode* FromNode = NodeGraphUtils::FindNodeByGuid(Graph, FromNodeId, Error);
		if (!FromNode) { return FAgentMcpToolResult::Error(Error); }
		UEdGraphNode* ToNode = NodeGraphUtils::FindNodeByGuid(Graph, ToNodeId, Error);
		if (!ToNode) { return FAgentMcpToolResult::Error(Error); }
		// from_pin: prefer Output; to_pin: prefer Input.
		// Non-null return from FindPin is usable; ignore fallback warning (schema CanCreateConnection is authoritative).
		UEdGraphPin* FromPin = NodeGraphUtils::FindPin(FromNode, FromPinName, EGPD_Output, Error);
		if (!FromPin) { return FAgentMcpToolResult::Error(Error); }
		UEdGraphPin* ToPin = NodeGraphUtils::FindPin(ToNode, ToPinName, EGPD_Input, Error);
		if (!ToPin) { return FAgentMcpToolResult::Error(Error); }

		// CanCreateConnection is read-only — check before transaction.
		const UEdGraphSchema* Schema = Graph->GetSchema();
		const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Connection rejected: %s"), *Response.Message.ToString()));
		}

		// Fix 2: snapshot links that will be broken by BREAK_OTHERS responses before the transaction.
		TArray<TSharedPtr<FJsonValue>> BrokenLinks;
		const bool bBreaksA = Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB;
		const bool bBreaksB = Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB;
		auto SnapshotLinks = [&BrokenLinks](const UEdGraphPin* Pin)
		{
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
					Link->SetStringField(TEXT("node_id"), Linked->GetOwningNode()->NodeGuid.ToString());
					Link->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
					BrokenLinks.Add(MakeShared<FJsonValueObject>(Link));
				}
			}
		};
		if (bBreaksA) { SnapshotLinks(FromPin); }
		if (bBreaksB) { SnapshotLinks(ToPin); }

		// --- Mutation: open transaction only now ---
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "ConnectPins", "MCP: Connect Pins"));
		FromNode->Modify();
		ToNode->Modify();
		if (!Schema->TryCreateConnection(FromPin, ToPin))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(TEXT("TryCreateConnection failed after CanCreateConnection allowed it (schema conversion path?). Re-read the graph."));
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("connected"), true);
		// Fix 1: report conversion-node insertion.
		if (Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
		{
			Result->SetStringField(TEXT("note"), TEXT("type mismatch resolved via an automatic conversion node inserted into the graph; call read_graph to see it"));
		}
		// Fix 2: report broken links (BREAK_OTHERS takes priority over conversion note since responses are mutually exclusive).
		if (BrokenLinks.Num() > 0)
		{
			Result->SetArrayField(TEXT("broken_links"), BrokenLinks);
			Result->SetStringField(TEXT("note"), TEXT("existing connections were broken to make room for this link (single-connection pin); see broken_links"));
		}
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// -----------------------------------------------------------------------
	// set_pin_default
	// -----------------------------------------------------------------------
	FAgentMcpToolResult HandleSetPinDefault(const TSharedPtr<FJsonObject>& Args)
	{
		// --- Validate all args before opening a transaction ---
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		FString Error;
		if (!ResolveGraphArgs(Args, Blueprint, Graph, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}
		FString NodeId, PinName, Value;
		if (!Args->TryGetStringField(TEXT("node_id"), NodeId) ||
			!Args->TryGetStringField(TEXT("pin_name"), PinName) ||
			!Args->TryGetStringField(TEXT("value"), Value))
		{
			return FAgentMcpToolResult::Error(TEXT("set_pin_default requires node_id, pin_name and value (string)."));
		}
		UEdGraphNode* Node = NodeGraphUtils::FindNodeByGuid(Graph, NodeId, Error);
		if (!Node) { return FAgentMcpToolResult::Error(Error); }
		// Prefer Input pins for defaults; non-null is usable even if FindPin set a fallback warning.
		UEdGraphPin* Pin = NodeGraphUtils::FindPin(Node, PinName, EGPD_Input, Error);
		if (!Pin) { return FAgentMcpToolResult::Error(Error); }
		if (Pin->Direction != EGPD_Input)
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Pin '%s' is an output pin; defaults only apply to inputs."), *PinName));
		}

		// --- Mutation ---
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetPinDefault", "MCP: Set Pin Default"));
		Node->Modify();
		// TrySetDefaultValue is a virtual on UEdGraphSchema (EdGraphSchema.h:899),
		// overridden by UEdGraphSchema_K2 (EdGraphSchema_K2.h:540).
		// Calling through the base-class pointer is correct — no cast needed.
		// Fix 3: detect silent K2 rejection — TrySetDefaultValue has no return value; snapshot-and-compare.
		const FString OldDefault = Pin->DefaultValue;
		Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
		if (Pin->DefaultValue == OldDefault && OldDefault != Value)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Schema rejected default value '%s' for pin '%s' (type mismatch, exec pin, container pin, or by-ref pin)."),
				*Value, *PinName));
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("pin"), PinName);
		Result->SetStringField(TEXT("value"), Pin->DefaultValue);
		// Advisory: default is ignored by the compiler while the pin has live connections.
		if (Pin->LinkedTo.Num() > 0)
		{
			Result->SetStringField(TEXT("note"), TEXT("pin has live connections; default is ignored while connected"));
		}
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
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
		// MakeGraphArgsSchema always sets "properties"; extend that shared object directly.
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

	TSharedRef<FJsonObject> MakeConnectPinsSchema()
	{
		TSharedRef<FJsonObject> Schema = MakeGraphArgsSchema();
		TSharedRef<FJsonObject> Props = Schema->GetObjectField(TEXT("properties")).ToSharedRef();
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("NodeGuid of the source node (from read_graph or add_node node_id)"));
			Props->SetObjectField(TEXT("from_node_id"), P);
		}
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("Pin name on the source node; Output pins are preferred when names collide"));
			Props->SetObjectField(TEXT("from_pin"), P);
		}
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("NodeGuid of the destination node"));
			Props->SetObjectField(TEXT("to_node_id"), P);
		}
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("Pin name on the destination node; Input pins are preferred when names collide"));
			Props->SetObjectField(TEXT("to_pin"), P);
		}
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("from_node_id")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("from_pin")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("to_node_id")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("to_pin")));
		Schema->SetArrayField(TEXT("required"), Required);
		return Schema;
	}

	TSharedRef<FJsonObject> MakeSetPinDefaultSchema()
	{
		TSharedRef<FJsonObject> Schema = MakeGraphArgsSchema();
		TSharedRef<FJsonObject> Props = Schema->GetObjectField(TEXT("properties")).ToSharedRef();
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("NodeGuid of the node that owns the pin (from read_graph or add_node node_id)"));
			Props->SetObjectField(TEXT("node_id"), P);
		}
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("Name of the input pin whose default value to set, e.g. InString"));
			Props->SetObjectField(TEXT("pin_name"), P);
		}
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("type"), TEXT("string"));
			P->SetStringField(TEXT("description"), TEXT("New default value as a string (K2 schema converts to the pin's actual type)"));
			Props->SetObjectField(TEXT("value"), P);
		}
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("node_id")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("pin_name")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
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
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("connect_pins");
		Def.Description = TEXT("Connects two pins in a Blueprint graph. from_pin searches Output pins first; to_pin searches Input pins first. The K2 schema validates the connection and returns its rejection reason on failure. After connecting, call compile_blueprint to verify. Args: blueprint_path, from_node_id, from_pin, to_node_id, to_pin (all required); graph_name (optional, defaults to EventGraph).");
		Def.InputSchema = MakeConnectPinsSchema();
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleConnectPins);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_pin_default");
		Def.Description = TEXT("Sets the default value of an input pin. The value is a string and is converted by the K2 schema to the pin's actual type (bool: 'true'/'false', int: '42', float: '3.14', string: the literal text). Note: the default is ignored by the compiler while the pin has live connections. Args: blueprint_path, node_id, pin_name, value (all required); graph_name (optional).");
		Def.InputSchema = MakeSetPinDefaultSchema();
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetPinDefault);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
