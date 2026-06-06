#include "Tools/BehaviorTreeTools.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "Misc/PackageName.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Decorators/BTDecorator_BlackboardBase.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/OutputDevice.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "UObject/UnrealType.h"

// BTGraph is editor-only data; null it out so the editor rebuilds from the runtime tree on next open.
// Confirmed by FBehaviorTreeEditor::RestoreBehaviorTree: when BTGraph==nullptr, the editor creates a
// new graph and calls UpdateAsset(ClearDebuggerFlags|KeepRebuildCounter) -> SpawnMissingNodes(),
// which walks BT->RootNode and recreates all editor graph nodes.  This is the standard authoring
// path used by programmatically created BT assets.
#if WITH_EDITORONLY_DATA
#define INVALIDATE_BT_GRAPH(BT) do { (BT)->BTGraph = nullptr; } while(0)
#else
#define INVALIDATE_BT_GRAPH(BT) do {} while(0)
#endif

namespace
{
	using namespace AgentMcp;

	// -------------------------------------------------------------------------
	// Error-capture device for ImportText
	// -------------------------------------------------------------------------
	class FBtImportErrors final : public FOutputDevice
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
	// Asset loading helpers
	// -------------------------------------------------------------------------

	UBehaviorTree* LoadBT(const FString& BtPath, FString& OutError)
	{
		// Normalize: strip object suffix if present.
		FString PkgName = BtPath;
		{
			int32 Dot = INDEX_NONE;
			if (PkgName.FindChar(TEXT('.'), Dot)) { PkgName = PkgName.Left(Dot); }
		}
		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *BtPath);
		if (!BT)
		{
			// Try without suffix.
			const FString ShortName = FPackageName::GetShortName(PkgName);
			const FString ObjPath = PkgName + TEXT(".") + ShortName;
			BT = LoadObject<UBehaviorTree>(nullptr, *ObjPath);
		}
		if (!BT)
		{
			OutError = FString::Printf(TEXT("BehaviorTree not found: '%s'. Use list_assets to discover BT assets."), *BtPath);
		}
		return BT;
	}

	// -------------------------------------------------------------------------
	// Index-path helpers  ("" = root composite, "0" = child 0, "0/2" = child 0's child 2)
	// -------------------------------------------------------------------------

	/**
	 * Resolves an index_path to a (ParentComposite, ChildIndex) pair.
	 * Empty path means "the root slot" (ParentComposite=nullptr, ChildIndex=-1).
	 * "0"  means BT->RootNode->Children[0].
	 * "0/1" means BT->RootNode->Children[0].ChildComposite->Children[1].
	 * Returns false + OutError on bad path.
	 */
	bool ResolveIndexPath(UBehaviorTree* BT, const FString& IndexPath,
		UBTCompositeNode*& OutParent, int32& OutChildIdx, FString& OutError)
	{
		if (IndexPath.IsEmpty())
		{
			// Root slot — caller is setting BT->RootNode directly.
			OutParent   = nullptr;
			OutChildIdx = -1;
			return true;
		}

		TArray<FString> Parts;
		IndexPath.ParseIntoArray(Parts, TEXT("/"), /*bCullEmpty=*/true);

		UBTCompositeNode* Current = BT->RootNode;
		if (!Current)
		{
			OutError = TEXT("BehaviorTree has no RootNode yet; add a root Composite first (parent_index_path=\"\").");
			return false;
		}

		// Walk all but the last segment descending into composites.
		for (int32 i = 0; i < Parts.Num() - 1; ++i)
		{
			int32 Idx = FCString::Atoi(*Parts[i]);
			if (Idx < 0 || Idx >= Current->Children.Num())
			{
				OutError = FString::Printf(
					TEXT("index_path segment [%d] out of range (node has %d children)."), Idx, Current->Children.Num());
				return false;
			}
			UBTCompositeNode* Next = Current->Children[Idx].ChildComposite;
			if (!Next)
			{
				OutError = FString::Printf(
					TEXT("index_path segment [%d] refers to a Task node, not a Composite. Cannot descend into it."), Idx);
				return false;
			}
			Current = Next;
		}

		OutParent   = Current;
		OutChildIdx = FCString::Atoi(*Parts.Last());
		return true;
	}

	// -------------------------------------------------------------------------
	// Class resolution: short name or full path, must be child of UBTNode
	// -------------------------------------------------------------------------

	UClass* ResolveBTNodeClass(const FString& ClassName, FString& OutError)
	{
		UClass* NodeClass = nullptr;
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
			OutError = FString::Printf(TEXT("Class '%s' not found. Use full path like /Script/AIModule.BTTask_MoveTo or short name BTTask_MoveTo."), *ClassName);
			return nullptr;
		}
		if (!NodeClass->IsChildOf(UBTNode::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a UBTNode subclass."), *ClassName);
			return nullptr;
		}
		return NodeClass;
	}

	// -------------------------------------------------------------------------
	// BlackboardKey resolution helper
	// Writes SelectedKeyName on the selector; logs available keys on failure.
	// -------------------------------------------------------------------------

	bool ResolveBlackboardKey(FBlackboardKeySelector& Selector, const FString& KeyName,
		UBlackboardData* BBAsset, FString& OutError)
	{
		if (!BBAsset)
		{
			OutError = TEXT("BehaviorTree has no BlackboardAsset assigned; cannot resolve BlackboardKey.");
			return false;
		}

		// Check that the key exists.
		const FBlackboard::FKey KeyID = BBAsset->GetKeyID(FName(*KeyName));
		if (KeyID == FBlackboard::InvalidKey)
		{
			// Build available-keys list for the error message.
			// Walk the full inheritance chain (BB->Parent) so inherited keys are listed too —
			// GetKeyID already searches parents, so the error message must match its search space.
			FString Available;
			for (UBlackboardData* BB = BBAsset; BB != nullptr; BB = BB->Parent)
			{
				for (const FBlackboardEntry& Entry : BB->Keys)
				{
					Available += (Available.IsEmpty() ? TEXT("") : TEXT(", ")) + Entry.EntryName.ToString();
				}
			}
			OutError = FString::Printf(
				TEXT("Blackboard key '%s' not found in '%s' (including parent BBs). Available keys: [%s]."),
				*KeyName, *BBAsset->GetName(), *Available);
			return false;
		}

		Selector.SelectedKeyName = FName(*KeyName);
		Selector.ResolveSelectedKey(*BBAsset);
		return true;
	}

	// -------------------------------------------------------------------------
	// Apply a properties map (string->string) to a UBTNode via ImportText.
	// FBlackboardKeySelector fields are handled specially: value is a key name.
	// -------------------------------------------------------------------------

	bool ApplyNodeProperties(UBTNode* Node, const TSharedPtr<FJsonObject>& Props,
		UBlackboardData* BBAsset, FString& OutError)
	{
		if (!Props.IsValid()) { return true; }

		for (const auto& Pair : Props->Values)
		{
			const FString& PropName = Pair.Key;
			FString PropValue;
			if (!Pair.Value->TryGetString(PropValue))
			{
				// Accept numbers/bools serialised as JSON too.
				PropValue = Pair.Value->AsString();
			}

			// Find the property on the node class.
			FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Prop)
			{
				OutError = FString::Printf(TEXT("Property '%s' not found on '%s'."), *PropName, *Node->GetClass()->GetName());
				return false;
			}

			// Special handling: FBlackboardKeySelector — value is a key name string.
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				if (StructProp->Struct == FBlackboardKeySelector::StaticStruct())
				{
					FBlackboardKeySelector* Selector =
						StructProp->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);
					if (!ResolveBlackboardKey(*Selector, PropValue, BBAsset, OutError))
					{
						return false;
					}
					continue;
				}
			}

			// General path: ImportText_Direct.
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
			FBtImportErrors Errors;
			const TCHAR* ImportResult = Prop->ImportText_Direct(*PropValue, ValuePtr, Node, PPF_None, &Errors);
			if (!ImportResult)
			{
				OutError = FString::Printf(TEXT("Failed to set property '%s' = '%s' on '%s'%s%s"),
					*PropName, *PropValue, *Node->GetClass()->GetName(),
					Errors.Captured.IsEmpty() ? TEXT("") : TEXT(": "), *Errors.Captured);
				return false;
			}
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// Build the index_path string for a node
	// -------------------------------------------------------------------------

	// Build the full slash-separated index_path for a newly inserted child.
	// ParentIndexPath is the resolved path of the parent composite ("" for root, "0", "0/1", etc.).
	// The result is directly usable in subsequent add_bt_node / add_bt_decorator calls.
	FString BuildIndexPath(const FString& ParentIndexPath, int32 ChildIdx)
	{
		if (ParentIndexPath.IsEmpty())
		{
			return FString::FromInt(ChildIdx);
		}
		return ParentIndexPath + TEXT("/") + FString::FromInt(ChildIdx);
	}

	// -------------------------------------------------------------------------
	// read_bt helpers: recursive node serialization
	// -------------------------------------------------------------------------

	TSharedRef<FJsonObject> SerializeNode(UBTNode* Node, const FString& IndexPath)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Obj->SetStringField(TEXT("name"), Node->GetNodeName());
		Obj->SetStringField(TEXT("index_path"), IndexPath);

		// Decorators (on composite children — handled by parent; here we handle on-node aux).
		// Services.
		if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			// Services on this composite.
			TArray<TSharedPtr<FJsonValue>> ServiceArray;
			for (UBTService* Svc : Composite->Services)
			{
				if (!Svc) continue;
				TSharedRef<FJsonObject> SvcObj = MakeShared<FJsonObject>();
				SvcObj->SetStringField(TEXT("class"), Svc->GetClass()->GetName());
				ServiceArray.Add(MakeShared<FJsonValueObject>(SvcObj));
			}
			Obj->SetArrayField(TEXT("services"), ServiceArray);

			// Children, recursively.
			TArray<TSharedPtr<FJsonValue>> ChildArray;
			for (int32 i = 0; i < Composite->Children.Num(); ++i)
			{
				const FBTCompositeChild& Child = Composite->Children[i];
				const FString ChildPath = IndexPath.IsEmpty()
					? FString::FromInt(i)
					: IndexPath + TEXT("/") + FString::FromInt(i);

				UBTNode* ChildNode = Child.ChildComposite
					? static_cast<UBTNode*>(Child.ChildComposite)
					: static_cast<UBTNode*>(Child.ChildTask);
				if (!ChildNode) { continue; }

				TSharedRef<FJsonObject> ChildObj = SerializeNode(ChildNode, ChildPath);

				// Decorators on the child-connection.
				TArray<TSharedPtr<FJsonValue>> DecoratorArray;
				for (UBTDecorator* Dec : Child.Decorators)
				{
					if (!Dec) continue;
					TSharedRef<FJsonObject> DecObj = MakeShared<FJsonObject>();
					DecObj->SetStringField(TEXT("class"), Dec->GetClass()->GetName());

					// Reflect FlowAbortMode via the property (public getter returns raw value).
					const TEnumAsByte<EBTFlowAbortMode::Type> AbortMode = Dec->GetFlowAbortMode();
					DecObj->SetStringField(TEXT("abort_mode"),
						UBehaviorTreeTypes::DescribeFlowAbortMode(AbortMode));

					// Blackboard key name if applicable.
					if (UBTDecorator_BlackboardBase* BBDec = Cast<UBTDecorator_BlackboardBase>(Dec))
					{
						DecObj->SetStringField(TEXT("key"), BBDec->GetSelectedBlackboardKey().ToString());
					}

					DecoratorArray.Add(MakeShared<FJsonValueObject>(DecObj));
				}
				ChildObj->SetArrayField(TEXT("decorators"), DecoratorArray);

				ChildArray.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
			Obj->SetArrayField(TEXT("children"), ChildArray);
		}

		// Key property summary via reflection (MoveTo->BlackboardKey, etc.).
		TSharedRef<FJsonObject> KeyProps = MakeShared<FJsonObject>();
		for (TFieldIterator<FStructProperty> It(Node->GetClass()); It; ++It)
		{
			if (It->Struct == FBlackboardKeySelector::StaticStruct())
			{
				const FBlackboardKeySelector* Sel =
					It->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);
				KeyProps->SetStringField(It->GetName(), Sel->SelectedKeyName.ToString());
			}
		}
		Obj->SetObjectField(TEXT("key_props"), KeyProps);

		return Obj;
	}

	// -------------------------------------------------------------------------
	// Outer normalization helper
	//
	// Problem: BTs authored in the editor store their runtime-node Outers under the
	// editor graph sub-tree (BTGraph → UBehaviorTreeGraphNode → NodeInstance).
	// When we null out BTGraph the memory references remain valid (read_bt works),
	// but SavePackage walks the Outer chain to decide what belongs to the package:
	// any node whose Outer chain does NOT lead back to the UBehaviorTree asset is
	// silently dropped on save.  The result is a truncated .uasset (e.g. 10 KB
	// instead of 12 KB) and a "has missing decorator node" error on reload.
	//
	// Fix: before any write, recursively traverse the live runtime tree and Rename
	// every node whose Outer != BT to use BT as the new Outer.
	//
	// Rename notes:
	//   - nullptr name → keep existing object name (engine auto-renames on conflict).
	//   - REN_DontCreateRedirectors: we don't want leftover soft-reference redirectors.
	//   - REN_NonTransactional: the rename itself is NOT undoable.  We call this after
	//     BT->Modify() so the undo record for the BT exists; but the individual Rename
	//     ops are excluded from undo.  On Undo the node data reverts (Modify captured
	//     the serialized state) but the Outer pointer may not be restored — this is
	//     acceptable because in practice undo restores the entire BT state from the
	//     pre-Modify snapshot, which already had the old (broken) Outer.  The nodes
	//     remain accessible from the reverted snapshot regardless.
	// -------------------------------------------------------------------------

	void NormalizeBTNodeOuters(UBehaviorTree* BT, UBTCompositeNode* Composite)
	{
		if (!Composite) { return; }

		auto FixOuter = [BT](UBTNode* Node)
		{
			if (Node && Node->GetOuter() != BT)
			{
				Node->Rename(nullptr, BT, REN_DontCreateRedirectors | REN_NonTransactional);
			}
		};

		// Fix the composite itself.
		FixOuter(Composite);

		// Fix services on this composite.
		for (UBTService* Svc : Composite->Services)
		{
			FixOuter(Svc);
		}

		// Recurse into children.
		for (FBTCompositeChild& Child : Composite->Children)
		{
			// Fix decorators on the child connection.
			for (UBTDecorator* Dec : Child.Decorators)
			{
				FixOuter(Dec);
			}

			if (Child.ChildComposite)
			{
				NormalizeBTNodeOuters(BT, Child.ChildComposite);
			}
			else if (Child.ChildTask)
			{
				FixOuter(Child.ChildTask);
			}
		}
	}

	/** Entry point: normalizes all runtime nodes in BT to have BT as their Outer.
	 *  Call after BT->Modify() and before any structural edits + INVALIDATE_BT_GRAPH. */
	void NormalizeBTNodeOuters(UBehaviorTree* BT)
	{
		if (!BT || !BT->RootNode) { return; }
		NormalizeBTNodeOuters(BT, BT->RootNode);
	}

	// -------------------------------------------------------------------------
	// Tool handlers
	// -------------------------------------------------------------------------

	FAgentMcpToolResult HandleReadBT(const TSharedPtr<FJsonObject>& Args)
	{
		FString BtPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("bt_path"), BtPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'bt_path'."));
		}

		FString Error;
		UBehaviorTree* BT = LoadBT(BtPath, Error);
		if (!BT) { return FAgentMcpToolResult::Error(Error); }

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

		// Blackboard.
		Result->SetStringField(TEXT("blackboard"),
			BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : TEXT(""));

		// Tree.
		if (BT->RootNode)
		{
			Result->SetObjectField(TEXT("tree"), SerializeNode(BT->RootNode, TEXT("")));
		}
		else
		{
			Result->SetField(TEXT("tree"), MakeShared<FJsonValueNull>());
		}

		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleAddBTNode(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("add_bt_node requires arguments."));
		}

		FString BtPath, NodeClassName, ParentIndexPath;
		Args->TryGetStringField(TEXT("bt_path"), BtPath);
		Args->TryGetStringField(TEXT("node_class"), NodeClassName);
		Args->TryGetStringField(TEXT("parent_index_path"), ParentIndexPath);

		if (BtPath.IsEmpty() || NodeClassName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("add_bt_node requires 'bt_path' and 'node_class'."));
		}

		FString Error;
		UBehaviorTree* BT = LoadBT(BtPath, Error);
		if (!BT) { return FAgentMcpToolResult::Error(Error); }

		// Resolve node class.
		UClass* NodeClass = ResolveBTNodeClass(NodeClassName, Error);
		if (!NodeClass) { return FAgentMcpToolResult::Error(Error); }

		// Determine insert index.
		int32 InsertIndex = -1; // -1 = append
		{
			double InsertNum = -1.0;
			if (Args->TryGetNumberField(TEXT("insert_index"), InsertNum))
			{
				InsertIndex = static_cast<int32>(InsertNum);
			}
		}

		// Properties map (optional).
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		TSharedPtr<FJsonObject> PropsObj;
		if (Args->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr)
		{
			PropsObj = *PropsPtr;
		}

		// Resolve parent.
		// Convention:
		//   parent_index_path="" + Composite → set BT->RootNode
		//   parent_index_path="" + Task/non-Composite → add as child of existing RootNode
		//   parent_index_path="N" → add as child of RootNode->Children[N].ChildComposite
		//   parent_index_path="N/M" → descend further
		UBTCompositeNode* ParentComposite = nullptr;
		int32 ParentChildIdx = -1;

		if (ParentIndexPath.IsEmpty() && NodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
		{
			// Root-set path: ParentComposite stays nullptr, handled below.
		}
		else if (ParentIndexPath.IsEmpty())
		{
			// Add as child of existing root composite.
			if (!BT->RootNode)
			{
				return FAgentMcpToolResult::Error(
					TEXT("parent_index_path=\"\" for a non-Composite node requires RootNode to already exist. Add a root Composite first."));
			}
			ParentComposite = BT->RootNode;
		}
		else
		{
			if (!ResolveIndexPath(BT, ParentIndexPath, ParentComposite, ParentChildIdx, Error))
			{
				return FAgentMcpToolResult::Error(Error);
			}
			// ParentChildIdx here is the leaf index — that is the child slot to descend into as parent.
			// We need the composite AT that child slot to be the parent.
			if (ParentComposite && ParentChildIdx >= 0)
			{
				if (ParentChildIdx >= ParentComposite->Children.Num())
				{
					return FAgentMcpToolResult::Error(
						FString::Printf(TEXT("parent_index_path leaf index %d out of range (node has %d children)."),
							ParentChildIdx, ParentComposite->Children.Num()));
				}
				UBTCompositeNode* ChildComp = ParentComposite->Children[ParentChildIdx].ChildComposite;
				if (!ChildComp)
				{
					return FAgentMcpToolResult::Error(
						FString::Printf(TEXT("parent_index_path leaf [%d] refers to a Task node, not a Composite. Cannot add children to a Task."), ParentChildIdx));
				}
				ParentComposite = ChildComp;
			}
		}

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddBTNode", "MCP: Add BT Node"));
		BT->Modify();

		// Normalize existing runtime-node Outers BEFORE any edits.
		// Editor-authored BTs store node Outers under the BTGraph sub-tree; when we
		// null BTGraph those nodes' Outer chains no longer lead back to the BT package,
		// so SavePackage silently drops the whole sub-tree.  Rename them to BT first.
		NormalizeBTNodeOuters(BT);

		// Create the node with BT as outer (matches engine convention).
		UBTNode* NewNode = NewObject<UBTNode>(BT, NodeClass, NAME_None, RF_Transactional);
		if (!NewNode)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("NewObject failed for class '%s'."), *NodeClass->GetName()));
		}

		// Apply properties.
		if (!ApplyNodeProperties(NewNode, PropsObj, BT->BlackboardAsset, Error))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}

		FString NewIndexPath;

		if (!ParentComposite)
		{
			// Set as root node (Composite only — already validated above by path routing).
			// Guard: refuse to silently replace an existing root and orphan the old tree.
			if (BT->RootNode != nullptr)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(
					TEXT("BehaviorTree already has a RootNode; pass a parent_index_path to add children."));
			}
			BT->RootNode = Cast<UBTCompositeNode>(NewNode);
			NewIndexPath = TEXT("");
		}
		else
		{
			// Insert as child of ParentComposite.
			ParentComposite->Modify();

			// UBTService nodes attach to a composite's Services array, not Children.
			// Reject early to avoid adding a null-pointer child slot.
			if (NodeClass->IsChildOf(UBTService::StaticClass()))
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(
					TEXT("UBTService nodes attach to a composite's Services array, not Children; service support is not implemented yet."));
			}

			FBTCompositeChild ChildEntry;
			if (NodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
			{
				ChildEntry.ChildComposite = Cast<UBTCompositeNode>(NewNode);
			}
			else
			{
				ChildEntry.ChildTask = Cast<UBTTaskNode>(NewNode);
			}

			// Defensive: if both pointers are null (unknown future subclass slipped through),
			// cancel the transaction rather than silently adding an empty slot.
			if (!ChildEntry.ChildComposite && !ChildEntry.ChildTask)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(
					FString::Printf(TEXT("Class '%s' is not a Composite or Task node and cannot be added as a BT child."),
						*NodeClass->GetName()));
			}

			if (InsertIndex >= 0 && InsertIndex < ParentComposite->Children.Num())
			{
				ParentComposite->Children.Insert(ChildEntry, InsertIndex);
				NewIndexPath = BuildIndexPath(ParentIndexPath, InsertIndex);
			}
			else
			{
				const int32 AppendIdx = ParentComposite->Children.Add(ChildEntry);
				NewIndexPath = BuildIndexPath(ParentIndexPath, AppendIdx);
			}
		}

		// Invalidate BTGraph so editor rebuilds from runtime tree on next open.
		INVALIDATE_BT_GRAPH(BT);
		BT->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("index_path"), NewIndexPath);
		Result->SetStringField(TEXT("class"), NodeClass->GetName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleAddBTDecorator(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("add_bt_decorator requires arguments."));
		}

		FString BtPath, NodeIndexPath, DecoratorClassName;
		Args->TryGetStringField(TEXT("bt_path"), BtPath);
		Args->TryGetStringField(TEXT("node_index_path"), NodeIndexPath);
		Args->TryGetStringField(TEXT("decorator_class"), DecoratorClassName);

		if (BtPath.IsEmpty() || NodeIndexPath.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("add_bt_decorator requires 'bt_path' and 'node_index_path'."));
		}

		if (DecoratorClassName.IsEmpty())
		{
			DecoratorClassName = TEXT("BTDecorator_Blackboard");
		}

		FString Error;
		UBehaviorTree* BT = LoadBT(BtPath, Error);
		if (!BT) { return FAgentMcpToolResult::Error(Error); }

		// Resolve decorator class.
		UClass* DecClass = ResolveBTNodeClass(DecoratorClassName, Error);
		if (!DecClass) { return FAgentMcpToolResult::Error(Error); }
		if (!DecClass->IsChildOf(UBTDecorator::StaticClass()))
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Class '%s' is not a UBTDecorator subclass."), *DecoratorClassName));
		}

		// Resolve the parent composite + child index from the node_index_path.
		// node_index_path refers to a child slot — e.g. "0" means ParentComposite->Children[0].
		UBTCompositeNode* ParentComposite = nullptr;
		int32 ChildIdx = -1;
		if (!ResolveIndexPath(BT, NodeIndexPath, ParentComposite, ChildIdx, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}

		if (!ParentComposite)
		{
			return FAgentMcpToolResult::Error(
				TEXT("node_index_path must point to a child slot (e.g. \"0\"), not the root itself."));
		}

		if (ChildIdx < 0 || ChildIdx >= ParentComposite->Children.Num())
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("node_index_path child index %d out of range (parent has %d children)."),
					ChildIdx, ParentComposite->Children.Num()));
		}

		// Properties map (optional).
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		TSharedPtr<FJsonObject> PropsObj;
		if (Args->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr)
		{
			PropsObj = *PropsPtr;
		}

		// observer_aborts -> FlowAbortMode.
		FString ObserverAborts;
		Args->TryGetStringField(TEXT("observer_aborts"), ObserverAborts);

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddBTDecorator", "MCP: Add BT Decorator"));
		BT->Modify();

		// Normalize existing runtime-node Outers BEFORE any edits (same reason as add_bt_node).
		NormalizeBTNodeOuters(BT);

		ParentComposite->Modify();

		UBTDecorator* NewDecorator = NewObject<UBTDecorator>(BT, DecClass, NAME_None, RF_Transactional);
		if (!NewDecorator)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("NewObject failed for decorator class '%s'."), *DecoratorClassName));
		}

		// Apply observer_aborts to FlowAbortMode (common enough to deserve a dedicated parameter).
		if (!ObserverAborts.IsEmpty())
		{
			EBTFlowAbortMode::Type AbortMode = EBTFlowAbortMode::None;
			if (ObserverAborts.Equals(TEXT("none"), ESearchCase::IgnoreCase))
			{
				AbortMode = EBTFlowAbortMode::None;
			}
			else if (ObserverAborts.Equals(TEXT("self"), ESearchCase::IgnoreCase))
			{
				AbortMode = EBTFlowAbortMode::Self;
			}
			else if (ObserverAborts.Equals(TEXT("lower_priority"), ESearchCase::IgnoreCase))
			{
				AbortMode = EBTFlowAbortMode::LowerPriority;
			}
			else if (ObserverAborts.Equals(TEXT("both"), ESearchCase::IgnoreCase))
			{
				AbortMode = EBTFlowAbortMode::Both;
			}
			else
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(
					FString::Printf(TEXT("Invalid observer_aborts value '%s'. Valid values: none, self, lower_priority, both."), *ObserverAborts));
			}

			// Set via the property (FlowAbortMode is a protected UPROPERTY, accessible via reflection).
			FProperty* AbortModeProp = NewDecorator->GetClass()->FindPropertyByName(TEXT("FlowAbortMode"));
			if (AbortModeProp)
			{
				void* ValuePtr = AbortModeProp->ContainerPtrToValuePtr<void>(NewDecorator);
				TEnumAsByte<EBTFlowAbortMode::Type>* EnumPtr =
					static_cast<TEnumAsByte<EBTFlowAbortMode::Type>*>(ValuePtr);
				*EnumPtr = AbortMode;
			}
		}

		// Apply remaining properties (including BlackboardKey special handling).
		if (!ApplyNodeProperties(NewDecorator, PropsObj, BT->BlackboardAsset, Error))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}

		// Attach decorator to the child slot.
		FBTCompositeChild& ChildSlot = ParentComposite->Children[ChildIdx];
		ChildSlot.Decorators.Add(NewDecorator);

		// Invalidate BTGraph so editor rebuilds from runtime tree on next open.
		INVALIDATE_BT_GRAPH(BT);
		BT->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("node_index_path"), NodeIndexPath);
		Result->SetStringField(TEXT("decorator_class"), NewDecorator->GetClass()->GetName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AgentMcp::Tools::RegisterBehaviorTreeTools()
{
	// ── read_bt ──────────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("read_bt");
		Def.Description = TEXT("Reads the runtime structure of a BehaviorTree asset. Returns {blackboard, tree:{class, name, index_path, children:[...], decorators:[{class, abort_mode, key}], services:[{class}], key_props:{...}}}. tree is null when RootNode is not set.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> BtProp = MakeShared<FJsonObject>();
			BtProp->SetStringField(TEXT("type"), TEXT("string"));
			BtProp->SetStringField(TEXT("description"), TEXT("Package or object path of the BehaviorTree asset, e.g. /Game/AI/BT_Enemy."));
			Props->SetObjectField(TEXT("bt_path"), BtProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Props);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("bt_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleReadBT);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── add_bt_node ──────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_bt_node");
		Def.Description = TEXT(
			"Adds a Composite or Task node to a BehaviorTree asset. "
			"parent_index_path=\"\" + Composite = set root node; \"\" + Task = add child of existing root; "
			"\"0\" = add child under root->Children[0].ChildComposite; \"0/2\" descends further. "
			"node_class accepts short name (BTTask_MoveTo) or full path (/Script/AIModule.BTTask_MoveTo). "
			"properties map applies ImportText to the new node's UPROPERTY fields; FBlackboardKeySelector "
			"values are key names (e.g. \"TargetActor\") resolved against the BT's BlackboardAsset — "
			"error lists available keys if the key is absent. "
			"Invalidates BTGraph so the editor rebuilds from the runtime tree on next open. "
			"Returns {added:true, index_path, class}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			auto MakeStr = [](const TCHAR* Desc) -> TSharedRef<FJsonObject>
			{
				TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("type"), TEXT("string"));
				P->SetStringField(TEXT("description"), Desc);
				return P;
			};
			Props->SetObjectField(TEXT("bt_path"),          MakeStr(TEXT("Package or object path of the BehaviorTree asset.")));
			Props->SetObjectField(TEXT("parent_index_path"),MakeStr(TEXT("Slash-separated child index path to the parent slot. Empty string = root. \"0\" = first child of root. \"0/2\" = third child of root's first child.")));
			Props->SetObjectField(TEXT("node_class"),       MakeStr(TEXT("Short name or full path of the UBTNode subclass to create, e.g. BTTask_MoveTo or BTComposite_Selector.")));
			Props->SetObjectField(TEXT("insert_index"),     [](){ TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("integer")); P->SetStringField(TEXT("description"), TEXT("Zero-based insertion position in the parent's Children array. Omit to append.")); return P; }());
			{
				TSharedRef<FJsonObject> PP = MakeShared<FJsonObject>();
				PP->SetStringField(TEXT("type"), TEXT("object"));
				PP->SetStringField(TEXT("description"), TEXT("Property name→value map. Values are ImportText strings. FBlackboardKeySelector values are key names resolved against the BT's BlackboardAsset."));
				Props->SetObjectField(TEXT("properties"), PP);
			}
			Def.InputSchema->SetObjectField(TEXT("properties"), Props);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("bt_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("node_class")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddBTNode);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── add_bt_decorator ─────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_bt_decorator");
		Def.Description = TEXT(
			"Adds a decorator to a child slot in a BehaviorTree. "
			"node_index_path is the slash-separated path to the child slot (e.g. \"0\" for root's first child). "
			"decorator_class defaults to BTDecorator_Blackboard. "
			"observer_aborts: none|self|lower_priority|both (maps to EBTFlowAbortMode). "
			"properties map supports FBlackboardKeySelector with key name values. "
			"Invalidates BTGraph so the editor rebuilds from runtime tree on next open. "
			"Returns {added:true, node_index_path, decorator_class}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			auto MakeStr = [](const TCHAR* Desc) -> TSharedRef<FJsonObject>
			{
				TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("type"), TEXT("string"));
				P->SetStringField(TEXT("description"), Desc);
				return P;
			};
			Props->SetObjectField(TEXT("bt_path"),          MakeStr(TEXT("Package or object path of the BehaviorTree asset.")));
			Props->SetObjectField(TEXT("node_index_path"),  MakeStr(TEXT("Slash-separated child index path of the target child slot, e.g. \"0\" for root's first child.")));
			Props->SetObjectField(TEXT("decorator_class"),  MakeStr(TEXT("Short name or full path of the UBTDecorator subclass. Defaults to BTDecorator_Blackboard.")));
			Props->SetObjectField(TEXT("observer_aborts"),  MakeStr(TEXT("Flow abort mode: none, self, lower_priority, or both.")));
			{
				TSharedRef<FJsonObject> PP = MakeShared<FJsonObject>();
				PP->SetStringField(TEXT("type"), TEXT("object"));
				PP->SetStringField(TEXT("description"), TEXT("Property name→value map. FBlackboardKeySelector values are key names resolved against the BT's BlackboardAsset."));
				Props->SetObjectField(TEXT("properties"), PP);
			}
			Def.InputSchema->SetObjectField(TEXT("properties"), Props);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("bt_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("node_index_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddBTDecorator);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
