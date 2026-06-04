#include "Tools/ActorTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/PropertyBridge.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	using namespace AgentMcp;

	UWorld* GetEditorWorld(FString& OutError)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			OutError = TEXT("Editor world unavailable.");
		}
		return World;
	}

	AActor* ResolveActor(const TSharedPtr<FJsonObject>& Args, FString& OutError)
	{
		FString ActorPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_path"), ActorPath))
		{
			OutError = TEXT("Missing required string argument 'actor_path' (from spawn_actor/query_actors).");
			return nullptr;
		}
		AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
		if (!Actor || !IsValid(Actor))
		{
			OutError = FString::Printf(TEXT("Actor not found (or pending kill): '%s'. Call query_actors for live paths."), *ActorPath);
			return nullptr;
		}
		return Actor;
	}

	/** Reads a {x, y, z} sub-object from Args. Returns false if the field is absent (absent = not provided, caller skips it). */
	bool ParseVector(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* SubObj = nullptr;
		if (!Args.IsValid() || !Args->TryGetObjectField(Field, SubObj) || !SubObj || !SubObj->IsValid())
		{
			return false;
		}
		double X = 0.0, Y = 0.0, Z = 0.0;
		(*SubObj)->TryGetNumberField(TEXT("x"), X);
		(*SubObj)->TryGetNumberField(TEXT("y"), Y);
		(*SubObj)->TryGetNumberField(TEXT("z"), Z);
		Out = FVector(X, Y, Z);
		return true;
	}

	/** Reads a {pitch, yaw, roll} sub-object from Args. Returns false if absent. */
	bool ParseRotator(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FRotator& Out)
	{
		const TSharedPtr<FJsonObject>* SubObj = nullptr;
		if (!Args.IsValid() || !Args->TryGetObjectField(Field, SubObj) || !SubObj || !SubObj->IsValid())
		{
			return false;
		}
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		(*SubObj)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*SubObj)->TryGetNumberField(TEXT("yaw"), Yaw);
		(*SubObj)->TryGetNumberField(TEXT("roll"), Roll);
		Out = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	FAgentMcpToolResult HandleSpawnActor(const TSharedPtr<FJsonObject>& Args)
	{
		FString ClassName;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("class_name"), ClassName))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'class_name'."));
		}

		// Resolve: try full path first, then TryFindTypeSlow for short names.
		UClass* ActorClass = nullptr;
		if (ClassName.Contains(TEXT(".")))
		{
			ActorClass = FindObject<UClass>(nullptr, *ClassName);
		}
		if (!ActorClass)
		{
			ActorClass = UClass::TryFindTypeSlow<UClass>(ClassName);
		}
		if (!ActorClass)
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Class '%s' not found. Use a short name like 'Actor', 'Pawn', or a full path."), *ClassName));
		}
		if (!ActorClass->IsChildOf(AActor::StaticClass()))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Class '%s' is not a subclass of Actor."), *ClassName));
		}
		if (ActorClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Class '%s' is abstract and cannot be instantiated."), *ClassName));
		}

		FString Error;
		UWorld* World = GetEditorWorld(Error);
		if (!World)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		ParseVector(Args, TEXT("location"), Location);
		ParseRotator(Args, TEXT("rotation"), Rotation);
		ParseVector(Args, TEXT("scale"), Scale);

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SpawnActor", "MCP: Spawn Actor"));

		AActor* Actor = GEditor->AddActor(
			World->GetCurrentLevel(),
			ActorClass,
			FTransform(Rotation, Location, Scale),
			/*bSilent=*/ false,
			RF_Transactional);

		if (!Actor)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("GEditor->AddActor returned null for class '%s'. The editor world may not be fully initialized in this context."), *ClassName));
		}

		FString Label;
		if (Args->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
		{
			Actor->SetActorLabel(Label);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("class"), ActorClass->GetName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleSetActorTransform(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		AActor* Actor = ResolveActor(Args, Error);
		if (!Actor)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		FVector Location;
		FRotator Rotation;
		FVector Scale;
		const bool bHasLocation  = ParseVector(Args, TEXT("location"), Location);
		const bool bHasRotation  = ParseRotator(Args, TEXT("rotation"), Rotation);
		const bool bHasScale     = ParseVector(Args, TEXT("scale"), Scale);

		if (!bHasLocation && !bHasRotation && !bHasScale)
		{
			return FAgentMcpToolResult::Error(TEXT("set_actor_transform requires at least one of: location {x,y,z}, rotation {pitch,yaw,roll}, scale {x,y,z}."));
		}

		const FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetActorTransform", "MCP: Set Actor Transform"));
		Actor->Modify();

		if (bHasLocation)  { Actor->SetActorLocation(Location); }
		if (bHasRotation)  { Actor->SetActorRotation(Rotation); }
		if (bHasScale)     { Actor->SetActorScale3D(Scale); }

		FString ActorPath;
		Args->TryGetStringField(TEXT("actor_path"), ActorPath);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("actor_path"), ActorPath);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleSetActorProperty(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		AActor* Actor = ResolveActor(Args, Error);
		if (!Actor)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		FString PropertyName, Value;
		if (!Args->TryGetStringField(TEXT("property"), PropertyName) || !Args->TryGetStringField(TEXT("value"), Value))
		{
			return FAgentMcpToolResult::Error(TEXT("set_actor_property requires 'property' and 'value' strings."));
		}

		// Non-const transaction — Cancel on failure so no empty undo entry pollutes the stack.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetActorProperty", "MCP: Set Actor Property"));
		Actor->Modify();

		// Actor instances: EditInstanceOnly is legal (bRejectTemplateDisabled = false).
		if (!PropertyBridge::SetPropertyFromString(Actor, PropertyName, Value, Error, /*bRejectTemplateDisabled=*/false))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}

		FString ReadBack, Type;
		PropertyBridge::GetPropertyAsString(Actor, PropertyName, ReadBack, Type, Error);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), ReadBack);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleQueryActors(const TSharedPtr<FJsonObject>& Args)
	{
		FString ClassName;
		FString LabelContains;
		int32 Limit = 100;

		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("class_name"), ClassName);
			Args->TryGetStringField(TEXT("label_contains"), LabelContains);
			double LimitNumber = 0.0;
			if (Args->TryGetNumberField(TEXT("limit"), LimitNumber))
			{
				Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 1000);
			}
		}

		// Optional class filter.
		UClass* FilterClass = nullptr;
		if (!ClassName.IsEmpty())
		{
			FilterClass = UClass::TryFindTypeSlow<UClass>(ClassName);
			if (!FilterClass)
			{
				return FAgentMcpToolResult::Error(FString::Printf(TEXT("class_name '%s' not found."), *ClassName));
			}
		}

		FString Error;
		UWorld* World = GetEditorWorld(Error);
		if (!World)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		TArray<TSharedPtr<FJsonValue>> ActorArray;
		int32 Total = 0;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor)) { continue; }
			if (FilterClass && !Actor->IsA(FilterClass)) { continue; }
			if (!LabelContains.IsEmpty() && !Actor->GetActorLabel().Contains(LabelContains, ESearchCase::IgnoreCase)) { continue; }

			++Total;
			if (ActorArray.Num() < Limit)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Entry->SetStringField(TEXT("actor_path"), Actor->GetPathName());
				Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				ActorArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("returned"), ActorArray.Num());
		Result->SetArrayField(TEXT("actors"), ActorArray);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleDestroyActor(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		AActor* Actor = ResolveActor(Args, Error);
		if (!Actor)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// Capture identity before destruction (can't access actor after destroy).
		const FString Label = Actor->GetActorLabel();
		const FString ClassName = Actor->GetClass()->GetName();

		FString Error2;
		UWorld* World = GetEditorWorld(Error2);
		if (!World)
		{
			return FAgentMcpToolResult::Error(Error2);
		}

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "DestroyActor", "MCP: Destroy Actor"));

		const bool bDestroyed = World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		if (!bDestroyed)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("EditorDestroyActor returned false for '%s'. The actor may be protected or the level locked."), *Label));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("destroyed"), true);
		Result->SetStringField(TEXT("label"), Label);
		Result->SetStringField(TEXT("class"), ClassName);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

void AgentMcp::Tools::RegisterActorTools()
{
	// ── spawn_actor ───────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("spawn_actor");
		Def.Description = TEXT("Spawns an actor of the given class in the editor world. The spawn is wrapped in an undo transaction. class_name must not be abstract. Returns {actor_path, label, class}. actor_path is the path needed by set_actor_transform, set_actor_property, and destroy_actor.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> ClassProp = MakeShared<FJsonObject>();
			ClassProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassProp->SetStringField(TEXT("description"), TEXT("Short class name (e.g. Actor, Pawn) or full path (e.g. /Script/Engine.Actor). Must be a concrete (non-abstract) AActor subclass."));
			Props->SetObjectField(TEXT("class_name"), ClassProp);

			TSharedRef<FJsonObject> LabelProp = MakeShared<FJsonObject>();
			LabelProp->SetStringField(TEXT("type"), TEXT("string"));
			LabelProp->SetStringField(TEXT("description"), TEXT("Optional display label shown in the Outliner."));
			Props->SetObjectField(TEXT("label"), LabelProp);

			TSharedRef<FJsonObject> LocProp = MakeShared<FJsonObject>();
			LocProp->SetStringField(TEXT("type"), TEXT("object"));
			LocProp->SetStringField(TEXT("description"), TEXT("World-space location {x, y, z} in Unreal units (cm). Defaults to origin."));
			Props->SetObjectField(TEXT("location"), LocProp);

			TSharedRef<FJsonObject> RotProp = MakeShared<FJsonObject>();
			RotProp->SetStringField(TEXT("type"), TEXT("object"));
			RotProp->SetStringField(TEXT("description"), TEXT("Rotation {pitch, yaw, roll} in degrees. Defaults to zero rotation."));
			Props->SetObjectField(TEXT("rotation"), RotProp);

			TSharedRef<FJsonObject> ScaleProp = MakeShared<FJsonObject>();
			ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
			ScaleProp->SetStringField(TEXT("description"), TEXT("Scale {x, y, z}. Defaults to (1,1,1)."));
			Props->SetObjectField(TEXT("scale"), ScaleProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("class_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSpawnActor);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── set_actor_transform ───────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_actor_transform");
		Def.Description = TEXT("Moves/rotates/scales an actor in the editor world. Provide any combination of location {x,y,z}, rotation {pitch,yaw,roll}, scale {x,y,z}. Only supplied components are changed. Wrapped in an undo transaction. Returns {set:true, actor_path}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Actor path returned by spawn_actor or query_actors."));
			Props->SetObjectField(TEXT("actor_path"), PathProp);

			TSharedRef<FJsonObject> LocProp = MakeShared<FJsonObject>();
			LocProp->SetStringField(TEXT("type"), TEXT("object"));
			LocProp->SetStringField(TEXT("description"), TEXT("New world-space location {x, y, z} in cm."));
			Props->SetObjectField(TEXT("location"), LocProp);

			TSharedRef<FJsonObject> RotProp = MakeShared<FJsonObject>();
			RotProp->SetStringField(TEXT("type"), TEXT("object"));
			RotProp->SetStringField(TEXT("description"), TEXT("New rotation {pitch, yaw, roll} in degrees."));
			Props->SetObjectField(TEXT("rotation"), RotProp);

			TSharedRef<FJsonObject> ScaleProp = MakeShared<FJsonObject>();
			ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
			ScaleProp->SetStringField(TEXT("description"), TEXT("New scale {x, y, z}."));
			Props->SetObjectField(TEXT("scale"), ScaleProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("actor_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetActorTransform);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── set_actor_property ────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_actor_property");
		Def.Description = TEXT("Sets an EditAnywhere property on a live actor instance via UE ImportText (True/False, 42, (X=1,Y=2,Z=3), /Script/... paths). Only EditAnywhere/EditInstanceOnly/EditDefaultsOnly properties are accepted. Wrapped in an undo transaction. Returns {set:true, property, value (readback)}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Actor path from spawn_actor or query_actors."));
			Props->SetObjectField(TEXT("actor_path"), PathProp);

			TSharedRef<FJsonObject> PropProp = MakeShared<FJsonObject>();
			PropProp->SetStringField(TEXT("type"), TEXT("string"));
			PropProp->SetStringField(TEXT("description"), TEXT("Case-sensitive C++ property name on the actor, e.g. bCanBeDamaged."));
			Props->SetObjectField(TEXT("property"), PropProp);

			TSharedRef<FJsonObject> ValProp = MakeShared<FJsonObject>();
			ValProp->SetStringField(TEXT("type"), TEXT("string"));
			ValProp->SetStringField(TEXT("description"), TEXT("Value in UE ImportText format: True/False (bool), 42 (int), 3.14 (float), (X=1,Y=2,Z=3) (vector)."));
			Props->SetObjectField(TEXT("value"), ValProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("actor_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("property")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetActorProperty);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── query_actors ──────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("query_actors");
		Def.Description = TEXT("Lists actors in the editor world with optional class and label filters. Returns {total, returned, actors:[{label, actor_path, class}]}. actor_path values are valid inputs for set_actor_transform, set_actor_property, and destroy_actor.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> ClassProp = MakeShared<FJsonObject>();
			ClassProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassProp->SetStringField(TEXT("description"), TEXT("Optional class filter; only actors of this type (or subclass) are returned."));
			Props->SetObjectField(TEXT("class_name"), ClassProp);

			TSharedRef<FJsonObject> LabelProp = MakeShared<FJsonObject>();
			LabelProp->SetStringField(TEXT("type"), TEXT("string"));
			LabelProp->SetStringField(TEXT("description"), TEXT("Optional case-insensitive substring filter on the actor label."));
			Props->SetObjectField(TEXT("label_contains"), LabelProp);

			TSharedRef<FJsonObject> LimitProp = MakeShared<FJsonObject>();
			LimitProp->SetStringField(TEXT("type"), TEXT("integer"));
			LimitProp->SetStringField(TEXT("description"), TEXT("Max number of actors to return (1-1000, default 100)."));
			Props->SetObjectField(TEXT("limit"), LimitProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleQueryActors);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── destroy_actor ─────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("destroy_actor");
		Def.Description = TEXT("DESTROYS the actor permanently from the editor world. The destruction is wrapped in an undo transaction (Ctrl+Z restores it). Returns {destroyed:true, label, class} echoing the actor identity before removal. Requires Destructive permission tier.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Actor path from spawn_actor or query_actors."));
			Props->SetObjectField(TEXT("actor_path"), PathProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("actor_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::Destructive;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleDestroyActor);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
