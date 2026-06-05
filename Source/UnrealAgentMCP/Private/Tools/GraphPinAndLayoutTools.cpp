#include "Tools/NodeGraphTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"

namespace
{
	using namespace AgentMcp;

	/** Shared arg unpacking: blueprint_path (required) + graph_name (optional). */
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

		// Dispatch on pin category: class/object/interface pins use TrySetDefaultObject;
		// soft-reference pins re-route through TrySetDefaultValue (engine handles path string internally);
		// all other pins use the existing snapshot-compare TrySetDefaultValue path.
		const FName Cat = Pin->PinType.PinCategory;
		UClass* MetaClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());

		if (Cat == UEdGraphSchema_K2::PC_Class)
		{
			// Value must be a /Script/Module.Class or /Game/Path/Asset.Asset_C path.
			UClass* ResolvedClass = LoadObject<UClass>(nullptr, *Value);
			if (!ResolvedClass)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Could not load class '%s' for class pin '%s'. Use a /Script/Module.Class native class or /Game/Path/Asset.Asset_C generated class."),
					*Value, *PinName));
			}
			if (MetaClass && !ResolvedClass->IsChildOf(MetaClass))
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Class '%s' is not a subclass of '%s' required by pin '%s'."),
					*ResolvedClass->GetName(), *MetaClass->GetName(), *PinName));
			}
			Pin->Modify();
			Graph->GetSchema()->TrySetDefaultObject(*Pin, ResolvedClass);
			if (Pin->DefaultObject != ResolvedClass)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Schema rejected class default '%s' on pin '%s'."),
					*Value, *PinName));
			}
		}
		else if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Interface)
		{
			UObject* ResolvedObj = LoadObject<UObject>(nullptr, *Value);
			if (!ResolvedObj)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Could not load object '%s' for pin '%s'."),
					*Value, *PinName));
			}
			if (MetaClass && !ResolvedObj->GetClass()->IsChildOf(MetaClass)
				&& !(Cat == UEdGraphSchema_K2::PC_Interface && ResolvedObj->GetClass()->ImplementsInterface(MetaClass)))
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Object '%s' (class %s) does not satisfy pin '%s' (needs %s)."),
					*Value, *ResolvedObj->GetClass()->GetName(), *PinName, *GetNameSafe(MetaClass)));
			}
			Pin->Modify();
			Graph->GetSchema()->TrySetDefaultObject(*Pin, ResolvedObj);
			if (Pin->DefaultObject != ResolvedObj)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Schema rejected object default '%s' on pin '%s'."),
					*Value, *PinName));
			}
		}
		else if (Cat == UEdGraphSchema_K2::PC_SoftClass || Cat == UEdGraphSchema_K2::PC_SoftObject)
		{
			// Soft-reference pins store the value in DefaultValue as a path string.
			const FString OldSoftDefault = Pin->DefaultValue;
			Pin->Modify();
			Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
			if (Pin->DefaultValue == OldSoftDefault && OldSoftDefault != Value)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Schema rejected soft-reference default '%s' on pin '%s'."),
					*Value, *PinName));
			}
		}
		else
		{
			// TrySetDefaultValue is a virtual on UEdGraphSchema (EdGraphSchema.h:899),
			// overridden by UEdGraphSchema_K2 (EdGraphSchema_K2.h:540).
			// Calling through the base-class pointer is correct — no cast needed.
			// Fix 3: detect silent K2 rejection — TrySetDefaultValue has no return value; snapshot-and-compare.
			const FString OldDefault = Pin->DefaultValue;
			Pin->Modify(); // record pin pre-state for undo, matching the object/class branches (T2 review finding)
			Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
			if (Pin->DefaultValue == OldDefault && OldDefault != Value)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Schema rejected default value '%s' for pin '%s' (type mismatch, exec pin, container pin, or by-ref pin)."),
					*Value, *PinName));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		// Shared success result — report DefaultValue for string pins, DefaultObject path for object pins.
		const bool bIsObjectPin = (Cat == UEdGraphSchema_K2::PC_Class || Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Interface);
		const FString ReportedValue = bIsObjectPin
			? (Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString())
			: Pin->DefaultValue;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("pin"), PinName);
		Result->SetStringField(TEXT("value"), ReportedValue);
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

	// -----------------------------------------------------------------------
	// auto_layout
	// -----------------------------------------------------------------------
	FAgentMcpToolResult HandleAutoLayout(const TSharedPtr<FJsonObject>& Args)
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		FString Error;
		if (!ResolveGraphArgs(Args, Blueprint, Graph, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// Collect live nodes and build predecessor sets (any input pin linked from another node).
		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node) { Nodes.Add(Node); }
		}
		TMap<UEdGraphNode*, TSet<UEdGraphNode*>> Predecessors;
		for (UEdGraphNode* Node : Nodes)
		{
			TSet<UEdGraphNode*>& Preds = Predecessors.FindOrAdd(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) { continue; }
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						Preds.Add(Linked->GetOwningNode());
					}
				}
			}
		}

		// Layer by longest-path depth via bounded relaxation (cycle leftovers go to a trailing layer).
		TMap<UEdGraphNode*, int32> Depth;
		for (UEdGraphNode* Node : Nodes)
		{
			if (Predecessors[Node].Num() == 0)
			{
				Depth.Add(Node, 0);
			}
		}
		// Relaxation passes: depth(node) = max(depth(pred)) + 1. Bounded passes avoid cycle hangs.
		for (int32 Pass = 0; Pass < Nodes.Num(); ++Pass)
		{
			bool bChanged = false;
			for (UEdGraphNode* Node : Nodes)
			{
				int32 MaxPred = -1;
				bool bAllKnown = Predecessors[Node].Num() > 0;
				for (UEdGraphNode* Pred : Predecessors[Node])
				{
					const int32* PredDepth = Depth.Find(Pred);
					if (!PredDepth) { bAllKnown = false; break; }
					MaxPred = FMath::Max(MaxPred, *PredDepth);
				}
				if (bAllKnown)
				{
					const int32 NewDepth = MaxPred + 1;
					int32* Existing = Depth.Find(Node);
					if (!Existing || *Existing != NewDepth)
					{
						Depth.Add(Node, NewDepth);
						bChanged = true;
					}
				}
			}
			if (!bChanged) { break; }
		}
		// Cycle leftovers: park after the deepest known layer, in stable order.
		int32 MaxDepth = 0;
		for (const TPair<UEdGraphNode*, int32>& Pair : Depth) { MaxDepth = FMath::Max(MaxDepth, Pair.Value); }
		for (UEdGraphNode* Node : Nodes)
		{
			if (!Depth.Contains(Node)) { Depth.Add(Node, ++MaxDepth); }
		}

		// Assign positions: x = depth * 420; y = stable per-layer stacking * 280.
		TMap<int32, int32> LayerCounts;
		constexpr int32 ColumnWidth = 420;
		constexpr int32 RowHeight = 280;

		const FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AutoLayout", "MCP: Auto Layout"));
		int32 LaidOut = 0;
		for (UEdGraphNode* Node : Nodes)
		{
			const int32 NodeDepth = Depth[Node];
			int32& RowIndex = LayerCounts.FindOrAdd(NodeDepth);
			Node->Modify();
			Node->NodePosX = NodeDepth * ColumnWidth;
			Node->NodePosY = RowIndex * RowHeight;
			++RowIndex;
			++LaidOut;
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSet<int32> DistinctLayers;
		for (const TPair<UEdGraphNode*, int32>& Pair : Depth) { DistinctLayers.Add(Pair.Value); }
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("laid_out"), LaidOut);
		Result->SetNumberField(TEXT("layers"), DistinctLayers.Num());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}
} // anonymous namespace

void AgentMcp::Tools::RegisterGraphPinAndLayoutTools()
{
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_pin_default");
		Def.Description = TEXT("Sets the default value of an input pin. The value is a string converted by the K2 schema to the pin's actual type (bool: 'true'/'false', int: '42', float: '3.14', string: the literal text). Class pins (PC_Class) accept '/Script/Module.Class' native paths or '/Game/Path/Asset.Asset_C' generated-class paths and are validated against the pin's required base class. Object/interface pins accept asset paths; soft-reference pins accept path strings. Note: the default is ignored by the compiler while the pin has live connections. Args: blueprint_path, node_id, pin_name, value (all required); graph_name (optional).");
		Def.InputSchema = MakeSetPinDefaultSchema();
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetPinDefault);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("auto_layout");
		Def.Description = TEXT("Rearranges all nodes in a graph into left-to-right layers by connection depth. Destructive to manual layout (undoable via Ctrl+Z). Returns {laid_out, layers}. Args: blueprint_path (required), graph_name (optional, defaults to EventGraph).");
		Def.InputSchema = MakeGraphArgsSchema();
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAutoLayout);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
