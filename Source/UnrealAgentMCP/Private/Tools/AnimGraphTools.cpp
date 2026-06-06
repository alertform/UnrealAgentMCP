#include "Tools/AnimGraphTools.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_Base.h"
#include "AnimationGraph.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/OutputDevice.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "UObject/UnrealType.h"

namespace
{
	using namespace AgentMcp;

	// -------------------------------------------------------------------------
	// Error capture device for ImportText
	// -------------------------------------------------------------------------
	class FAnimImportErrors final : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			if (!Captured.IsEmpty()) { Captured += TEXT("; "); }
			Captured += V;
		}
	};

	// -------------------------------------------------------------------------
	// Class resolution: short name or full path, must be child of UAnimGraphNode_Base
	// -------------------------------------------------------------------------
	UClass* ResolveAnimGraphNodeClass(const FString& ClassName, FString& OutError)
	{
		UClass* NodeClass = nullptr;

		// Full /Script/Module.ClassName path
		if (ClassName.Contains(TEXT(".")))
		{
			NodeClass = FindObject<UClass>(nullptr, *ClassName);
		}
		if (!NodeClass)
		{
			NodeClass = UClass::TryFindTypeSlow<UClass>(ClassName);
		}
		if (!NodeClass)
		{
			OutError = FString::Printf(
				TEXT("Class '%s' not found. Use full path like /Script/AnimGraph.AnimGraphNode_Slot or short name AnimGraphNode_Slot."),
				*ClassName);
			return nullptr;
		}
		if (!NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("Class '%s' is not a UAnimGraphNode_Base subclass. Only AnimGraph node classes are supported."),
				*ClassName);
			return nullptr;
		}
		return NodeClass;
	}

	// -------------------------------------------------------------------------
	// Load + validate UAnimBlueprint from path
	// -------------------------------------------------------------------------
	UAnimBlueprint* ResolveAnimBlueprint(const FString& Path, FString& OutError)
	{
		// Try the path as given first — handles transient objects (e.g. /Engine/Transient.ABP_Foo_0)
		// and already-loaded in-memory assets.
		UBlueprint* BP = FindObject<UBlueprint>(nullptr, *Path);

		if (!BP)
		{
			// Disk asset form: strip object suffix and rebuild as Package.ShortName.
			FString PkgName = Path;
			{
				int32 DotIdx = INDEX_NONE;
				if (PkgName.FindChar(TEXT('.'), DotIdx)) { PkgName = PkgName.Left(DotIdx); }
			}
			const FString ShortName = FPackageName::GetShortName(PkgName);
			const FString ObjPath   = PkgName + TEXT(".") + ShortName;

			BP = FindObject<UBlueprint>(nullptr, *ObjPath);
			if (!BP) { BP = LoadObject<UBlueprint>(nullptr, *ObjPath); }
		}

		if (!BP)
		{
			OutError = FString::Printf(
				TEXT("Blueprint not found: '%s'. Use list_assets or search_assets to discover AnimBlueprint assets."), *Path);
			return nullptr;
		}

		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(BP);
		if (!AnimBP)
		{
			OutError = FString::Printf(
				TEXT("'%s' is a %s, not a UAnimBlueprint. add_anim_graph_node only works on Animation Blueprints."),
				*Path, *BP->GetClass()->GetName());
			return nullptr;
		}
		return AnimBP;
	}

	// -------------------------------------------------------------------------
	// Resolve the AnimGraph by name (default "AnimGraph")
	// -------------------------------------------------------------------------
	UEdGraph* ResolveAnimGraph(UAnimBlueprint* AnimBP, const FString& GraphName, FString& OutError)
	{
		const FString TargetName = GraphName.IsEmpty() ? TEXT("AnimGraph") : GraphName;

		// AnimGraphs live in FunctionGraphs (the primary AnimGraph) or UbergraphPages.
		// The primary AnimGraph is typically in FunctionGraphs as "AnimGraph".
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetName() == TargetName)
			{
				return G;
			}
		}
		for (UEdGraph* G : AnimBP->UbergraphPages)
		{
			if (G && G->GetName() == TargetName)
			{
				return G;
			}
		}

		// Build available list for the error message.
		FString Available;
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G) { Available += (Available.IsEmpty() ? TEXT("") : TEXT(", ")) + G->GetName(); }
		}
		for (UEdGraph* G : AnimBP->UbergraphPages)
		{
			if (G) { Available += (Available.IsEmpty() ? TEXT("") : TEXT(", ")) + G->GetName(); }
		}

		OutError = FString::Printf(
			TEXT("AnimGraph '%s' not found in '%s'. Available graphs: [%s]."),
			*TargetName, *AnimBP->GetName(), *Available);
		return nullptr;
	}

	// -------------------------------------------------------------------------
	// Apply properties to an AnimGraphNode.
	// Supports two forms:
	//   "Node.SlotName" -> find property "SlotName" inside the Node struct member
	//   "Node"          -> ImportText the entire struct value (rarely needed)
	//   "OtherProp"     -> direct property on the node object
	// -------------------------------------------------------------------------
	bool ApplyAnimNodeProperties(UAnimGraphNode_Base* Node,
		const TSharedPtr<FJsonObject>& PropsObj,
		FString& OutError)
	{
		if (!PropsObj.IsValid()) { return true; }

		FAnimImportErrors ErrorCapture;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropsObj->Values)
		{
			const FString& Key   = Pair.Key;
			const FString  Value = Pair.Value.IsValid() ? Pair.Value->AsString() : FString();

			// Dot-path form: "Node.PropertyName" -> look inside the Node struct member
			if (Key.StartsWith(TEXT("Node.")))
			{
				const FString SubPropName = Key.Mid(5); // strip "Node."

				// Find the "Node" property on the editor node class
				FProperty* NodeProp = Node->GetClass()->FindPropertyByName(TEXT("Node"));
				if (!NodeProp)
				{
					OutError = FString::Printf(
						TEXT("AnimGraphNode '%s' has no 'Node' struct member. Cannot set '%s'."),
						*Node->GetClass()->GetName(), *Key);
					return false;
				}

				FStructProperty* NodeStructProp = CastField<FStructProperty>(NodeProp);
				if (!NodeStructProp)
				{
					OutError = FString::Printf(TEXT("'Node' on '%s' is not a struct property."), *Node->GetClass()->GetName());
					return false;
				}

				// Find the sub-property inside the struct
				FProperty* SubProp = NodeStructProp->Struct->FindPropertyByName(FName(*SubPropName));
				if (!SubProp)
				{
					OutError = FString::Printf(
						TEXT("Struct '%s' has no property '%s'. Check the UAnimNode struct definition."),
						*NodeStructProp->Struct->GetName(), *SubPropName);
					return false;
				}

				void* NodeStructPtr = NodeProp->ContainerPtrToValuePtr<void>(Node);
				void* SubValuePtr   = SubProp->ContainerPtrToValuePtr<void>(NodeStructPtr);

				ErrorCapture.Captured.Empty();
				const TCHAR* Remainder = SubProp->ImportText_Direct(*Value, SubValuePtr, Node, PPF_None, &ErrorCapture);
				if (!Remainder || !ErrorCapture.Captured.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("ImportText failed for '%s' = '%s': %s"),
						*Key, *Value, *ErrorCapture.Captured);
					return false;
				}
			}
			else
			{
				// Direct property on the editor node object
				FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*Key));
				if (!Prop)
				{
					OutError = FString::Printf(
						TEXT("Property '%s' not found on '%s'. Use 'Node.PropertyName' form for runtime struct members."),
						*Key, *Node->GetClass()->GetName());
					return false;
				}

				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
				ErrorCapture.Captured.Empty();
				const TCHAR* Remainder = Prop->ImportText_Direct(*Value, ValuePtr, Node, PPF_None, &ErrorCapture);
				if (!Remainder || !ErrorCapture.Captured.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("ImportText failed for '%s' = '%s': %s"),
						*Key, *Value, *ErrorCapture.Captured);
					return false;
				}
			}
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// Serialize one node's pins to a JSON array
	// -------------------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> PinsToJson(const UEdGraphNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) { continue; }
			TSharedRef<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"),      Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		return PinsArr;
	}

	// =========================================================================
	// add_anim_graph_node
	// =========================================================================
	FAgentMcpToolResult HandleAddAnimGraphNode(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required arguments."));
		}

		// --- blueprint_path (required) ---
		FString BlueprintPath;
		if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}

		// --- node_class (required) ---
		FString NodeClassName;
		if (!Args->TryGetStringField(TEXT("node_class"), NodeClassName))
		{
			return FAgentMcpToolResult::Error(
				TEXT("Missing required string argument 'node_class' (e.g. AnimGraphNode_Slot or /Script/AnimGraph.AnimGraphNode_Slot)."));
		}

		// --- graph_name (optional, default "AnimGraph") ---
		FString GraphName;
		Args->TryGetStringField(TEXT("graph_name"), GraphName);

		// --- pos_x / pos_y (optional) ---
		int32 PosX = 0, PosY = 0;
		double Num = 0.0;
		if (Args->TryGetNumberField(TEXT("pos_x"), Num)) { PosX = static_cast<int32>(Num); }
		if (Args->TryGetNumberField(TEXT("pos_y"), Num)) { PosY = static_cast<int32>(Num); }

		// --- Resolve AnimBlueprint ---
		FString Error;
		UAnimBlueprint* AnimBP = ResolveAnimBlueprint(BlueprintPath, Error);
		if (!AnimBP) { return FAgentMcpToolResult::Error(Error); }

		// --- Resolve AnimGraph ---
		UEdGraph* Graph = ResolveAnimGraph(AnimBP, GraphName, Error);
		if (!Graph) { return FAgentMcpToolResult::Error(Error); }

		// --- Resolve node class ---
		UClass* NodeClass = ResolveAnimGraphNodeClass(NodeClassName, Error);
		if (!NodeClass) { return FAgentMcpToolResult::Error(Error); }

		// --- Transaction + spawn ---
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddAnimGraphNode", "MCP: Add AnimGraph Node"));
		Graph->Modify();

		// SpawnNodeFromTemplate<T> is typed — we use the non-typed PerformAction path via
		// FEdGraphSchemaAction_NewNode with a template object of the resolved class.
		// This is the same internal path SpawnNodeFromTemplate<T> takes.
		UAnimGraphNode_Base* TemplateNode = NewObject<UAnimGraphNode_Base>(
			GetTransientPackage(), NodeClass, NAME_None, RF_Transient);
		if (!TemplateNode)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Failed to create template node of class '%s'."), *NodeClass->GetName()));
		}

		// Apply properties BEFORE spawning so the spawned node inherits them.
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (Args->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && (*PropsPtr).IsValid())
		{
			if (!ApplyAnimNodeProperties(TemplateNode, *PropsPtr, Error))
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(Error);
			}
		}

		// Use FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate (typed).
		// Because we need a typed T*, and we already have a UAnimGraphNode_Base*,
		// call the engine's static non-typed variant via CreateNode on the schema.
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(TEXT("Graph has no schema. Cannot spawn node."));
		}

		// Position the template
		TemplateNode->NodePosX = PosX;
		TemplateNode->NodePosY = PosY;

		// FEdGraphSchemaAction_NewNode::CreateNode is the engine API for programmatic node creation:
		// it calls PrepareForCopying, AddNode, NodeConstructionScript, AllocateDefaultPins, etc.
		UEdGraphNode* NewNode = FEdGraphSchemaAction_NewNode::CreateNode(Graph, nullptr,
			FVector2D(static_cast<float>(PosX), static_cast<float>(PosY)), TemplateNode);

		if (!NewNode)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Schema failed to create node of class '%s'. The graph may not support this node type."),
				*NodeClass->GetName()));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

		// --- Build result ---
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"),    true);
		Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
		Result->SetStringField(TEXT("class"),   NewNode->GetClass()->GetName());
		Result->SetArrayField(TEXT("pins"),     PinsToJson(NewNode));

		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// =========================================================================
	// register_skeleton_slot
	// =========================================================================
	FAgentMcpToolResult HandleRegisterSkeletonSlot(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required arguments."));
		}

		FString SkeletonPath;
		if (!Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'skeleton_path'."));
		}

		FString SlotName;
		if (!Args->TryGetStringField(TEXT("slot_name"), SlotName))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'slot_name'."));
		}

		FString GroupName = TEXT("DefaultGroup");
		Args->TryGetStringField(TEXT("group_name"), GroupName);

		// --- Resolve Skeleton ---
		// Try the path as-given first (handles transient objects like /Engine/Transient.SK_Foo_0).
		USkeleton* Skeleton = FindObject<USkeleton>(nullptr, *SkeletonPath);
		if (!Skeleton)
		{
			// Disk asset form: strip object suffix, rebuild as Package.ShortName.
			FString PkgName = SkeletonPath;
			{
				int32 DotIdx = INDEX_NONE;
				if (PkgName.FindChar(TEXT('.'), DotIdx)) { PkgName = PkgName.Left(DotIdx); }
			}
			const FString ShortName = FPackageName::GetShortName(PkgName);
			const FString ObjPath   = PkgName + TEXT(".") + ShortName;
			Skeleton = FindObject<USkeleton>(nullptr, *ObjPath);
			if (!Skeleton) { Skeleton = LoadObject<USkeleton>(nullptr, *ObjPath); }
		}
		if (!Skeleton)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Skeleton not found: '%s'. Use search_assets to discover Skeleton assets."), *SkeletonPath));
		}

		const FName SlotFName(*SlotName);
		const FName GroupFName(*GroupName);

		// --- Idempotency check: is this slot already registered? ---
		const bool bSlotExists = Skeleton->ContainsSlotName(SlotFName);

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "RegisterSkeletonSlot", "MCP: Register Skeleton Slot"));
		Skeleton->Modify();

		if (!bSlotExists)
		{
			// Ensure the group exists first (AddSlotGroupName is idempotent).
			Skeleton->AddSlotGroupName(GroupFName);

			// Register the slot node (returns true if newly added).
			Skeleton->RegisterSlotNode(SlotFName);

			// Assign the slot to the requested group.
			Skeleton->SetSlotGroupName(SlotFName, GroupFName);

			Skeleton->MarkPackageDirty();
		}
		else
		{
			// Slot already exists — no structural change, cancel the undo entry.
			Transaction.Cancel();
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("registered"),         !bSlotExists);
		Result->SetBoolField(TEXT("already_registered"), bSlotExists);
		Result->SetStringField(TEXT("slot_name"),         SlotName);
		Result->SetStringField(TEXT("group_name"),
			bSlotExists ? Skeleton->GetSlotGroupName(SlotFName).ToString() : GroupName);

		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AgentMcp::Tools::RegisterAnimGraphTools()
{
	// ── add_anim_graph_node ──────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_anim_graph_node");
		Def.Description = TEXT(
			"Adds an AnimGraph node (UAnimGraphNode_Base subclass) to an Animation Blueprint's AnimGraph. "
			"Use this to insert nodes like AnimGraphNode_Slot or AnimGraphNode_LayeredBoneBlend for "
			"upper-body layering workflows. "
			"node_class accepts short name (AnimGraphNode_Slot) or full path (/Script/AnimGraph.AnimGraphNode_Slot). "
			"properties map supports 'Node.PropertyName' dot-path form for runtime struct members "
			"(e.g. {\"Node.SlotName\":\"UpperBody\"}) and direct property names for editor-node fields. "
			"Returns {added, node_id (GUID), class, pins:[{name, direction}]}. "
			"Pins list lets you connect nodes via connect_pins immediately after. "
			"Requires a UAnimBlueprint asset — use search_assets with class filter AnimBlueprint.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
			BpProp->SetStringField(TEXT("type"), TEXT("string"));
			BpProp->SetStringField(TEXT("description"), TEXT("Package path of the AnimBlueprint, e.g. /Game/Characters/Mannequins/Animations/ABP_Manny."));
			Props->SetObjectField(TEXT("blueprint_path"), BpProp);

			TSharedRef<FJsonObject> GraphProp = MakeShared<FJsonObject>();
			GraphProp->SetStringField(TEXT("type"), TEXT("string"));
			GraphProp->SetStringField(TEXT("description"), TEXT("Graph name (default 'AnimGraph')."));
			Props->SetObjectField(TEXT("graph_name"), GraphProp);

			TSharedRef<FJsonObject> ClassProp = MakeShared<FJsonObject>();
			ClassProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassProp->SetStringField(TEXT("description"),
				TEXT("UAnimGraphNode_Base subclass. Short name (AnimGraphNode_Slot) or full path (/Script/AnimGraph.AnimGraphNode_Slot)."));
			Props->SetObjectField(TEXT("node_class"), ClassProp);

			TSharedRef<FJsonObject> PropsProp = MakeShared<FJsonObject>();
			PropsProp->SetStringField(TEXT("type"), TEXT("object"));
			PropsProp->SetStringField(TEXT("description"),
				TEXT("Optional property map. Use 'Node.SlotName' dot-path for runtime struct members, e.g. {\"Node.SlotName\":\"UpperBody\"}."));
			Props->SetObjectField(TEXT("properties"), PropsProp);

			TSharedRef<FJsonObject> PosXProp = MakeShared<FJsonObject>();
			PosXProp->SetStringField(TEXT("type"), TEXT("number"));
			PosXProp->SetStringField(TEXT("description"), TEXT("Node X position in graph (optional, default 0)."));
			Props->SetObjectField(TEXT("pos_x"), PosXProp);

			TSharedRef<FJsonObject> PosYProp = MakeShared<FJsonObject>();
			PosYProp->SetStringField(TEXT("type"), TEXT("number"));
			PosYProp->SetStringField(TEXT("description"), TEXT("Node Y position in graph (optional, default 0)."));
			Props->SetObjectField(TEXT("pos_y"), PosYProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("node_class")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier    = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddAnimGraphNode);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── register_skeleton_slot ───────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("register_skeleton_slot");
		Def.Description = TEXT(
			"Registers an animation slot name on a USkeleton asset so AnimGraph Slot nodes and montages "
			"can reference it. Idempotent: if the slot already exists the tool returns already_registered=true "
			"with no structural change. Use before placing AnimGraphNode_Slot nodes that reference a "
			"non-default slot (e.g. 'UpperBody' for layered-blend workflows). "
			"Returns {registered, already_registered, slot_name, group_name}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> SkelProp = MakeShared<FJsonObject>();
			SkelProp->SetStringField(TEXT("type"), TEXT("string"));
			SkelProp->SetStringField(TEXT("description"), TEXT("Package path of the Skeleton asset, e.g. /Game/Characters/Mannequins/Meshes/SK_Mannequin_Skeleton."));
			Props->SetObjectField(TEXT("skeleton_path"), SkelProp);

			TSharedRef<FJsonObject> SlotProp = MakeShared<FJsonObject>();
			SlotProp->SetStringField(TEXT("type"), TEXT("string"));
			SlotProp->SetStringField(TEXT("description"), TEXT("Slot name to register (e.g. 'UpperBody')."));
			Props->SetObjectField(TEXT("slot_name"), SlotProp);

			TSharedRef<FJsonObject> GroupProp = MakeShared<FJsonObject>();
			GroupProp->SetStringField(TEXT("type"), TEXT("string"));
			GroupProp->SetStringField(TEXT("description"), TEXT("Slot group name (default 'DefaultGroup')."));
			Props->SetObjectField(TEXT("group_name"), GroupProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("skeleton_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("slot_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier    = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleRegisterSkeletonSlot);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
