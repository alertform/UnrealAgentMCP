#include "Tools/AnimMontageTools.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	using namespace AgentMcp;

	// ---------------------------------------------------------------------------
	// Internal helpers
	// ---------------------------------------------------------------------------

	/**
	 * Resolves a UAnimSequenceBase from a package path.
	 * Accepts /Game/Foo/AS_Bar (no dot) and /Game/Foo/AS_Bar.AS_Bar forms.
	 */
	UAnimSequenceBase* ResolveAnimAsset(const FString& Path, FString& OutError)
	{
		// Normalise: strip object suffix if present.
		FString PackageName = Path;
		{
			int32 DotIdx = INDEX_NONE;
			if (PackageName.FindChar(TEXT('.'), DotIdx))
			{
				PackageName = PackageName.Left(DotIdx);
			}
		}

		const FString ShortName = FPackageName::GetShortName(PackageName);
		const FString ObjectPath = PackageName + TEXT(".") + ShortName;

		// FindObject covers transient / already-loaded assets.
		UAnimSequenceBase* Anim = FindObject<UAnimSequenceBase>(nullptr, *ObjectPath);
		if (!Anim)
		{
			Anim = LoadObject<UAnimSequenceBase>(nullptr, *ObjectPath);
		}
		if (!Anim)
		{
			OutError = FString::Printf(TEXT("AnimSequence not found or could not be loaded: '%s'. Check the path with search_assets."), *Path);
			return nullptr;
		}
		return Anim;
	}

	/**
	 * Resolves a UClass token to a UClass that IsChildOf(UAnimNotify::StaticClass()).
	 * Supports full paths (/Script/Engine.AnimNotify_PlaySound) and short names.
	 */
	UClass* ResolveAnimNotifyClass(const FString& Token, FString& OutError)
	{
		UClass* Class = nullptr;

		// Full path form: contains a dot separating module and class name.
		if (Token.Contains(TEXT(".")))
		{
			Class = FindObject<UClass>(nullptr, *Token);
			if (!Class)
			{
				Class = LoadClass<UAnimNotify>(nullptr, *Token);
			}
		}
		if (!Class)
		{
			Class = UClass::TryFindTypeSlow<UClass>(Token);
		}

		if (!Class)
		{
			OutError = FString::Printf(
				TEXT("Notify class '%s' not found. Use a full path like '/Script/Engine.AnimNotify_PlaySound' or a short name."), *Token);
			return nullptr;
		}
		if (!Class->IsChildOf(UAnimNotify::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("Class '%s' is not a UAnimNotify subclass. Only UAnimNotify subclasses are supported."), *Token);
			return nullptr;
		}
		return Class;
	}

	// ---------------------------------------------------------------------------
	// create_anim_montage
	// ---------------------------------------------------------------------------

	FAgentMcpToolResult HandleCreateAnimMontage(const TSharedPtr<FJsonObject>& Args)
	{
		FString SourcePath, AssetPath, SlotName = TEXT("DefaultSlot");

		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("source_animation"), SourcePath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'source_animation'."));
		}
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path'."));
		}
		Args->TryGetStringField(TEXT("slot_name"), SlotName);

		if (!AssetPath.StartsWith(TEXT("/")))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'asset_path' must be an absolute package path starting with '/'. Got: '%s'"), *AssetPath));
		}

		// Load source animation.
		FString LoadError;
		UAnimSequenceBase* SourceAnim = ResolveAnimAsset(SourcePath, LoadError);
		if (!SourceAnim)
		{
			return FAgentMcpToolResult::Error(LoadError);
		}

		UAnimSequence* SourceSeq = Cast<UAnimSequence>(SourceAnim);
		if (!SourceSeq)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'%s' is not a UAnimSequence (got %s). create_anim_montage requires an AnimSequence source."),
				*SourcePath, *SourceAnim->GetClass()->GetName()));
		}

		USkeleton* Skeleton = SourceSeq->GetSkeleton();
		if (!Skeleton)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Source animation '%s' has no skeleton. Cannot create a montage without a skeleton."), *SourcePath));
		}

		// Retrieve and validate optional cropping parameters.
		const float SourceLength = SourceSeq->GetPlayLength();

		double RawStartTime = 0.0;
		double RawEndTime   = static_cast<double>(SourceLength);
		Args->TryGetNumberField(TEXT("start_time"), RawStartTime);
		Args->TryGetNumberField(TEXT("end_time"),   RawEndTime);

		if (RawStartTime < 0.0)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'start_time' must be >= 0. Got: %.4f"), RawStartTime));
		}

		// Clamp end_time to source length (report actual value in response).
		const bool bEndTimeClamped = RawEndTime > static_cast<double>(SourceLength);
		const float EffectiveStartTime = static_cast<float>(RawStartTime);
		const float EffectiveEndTime   = FMath::Clamp(static_cast<float>(RawEndTime), 0.f, SourceLength);

		if (EffectiveEndTime - EffectiveStartTime <= 0.f)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'end_time' (%.4f) must be greater than 'start_time' (%.4f) after clamping."),
				EffectiveEndTime, EffectiveStartTime));
		}

		const float MontageLength = EffectiveEndTime - EffectiveStartTime;

		// Validate / create target package.
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);

		// Pre-check asset registry for on-disk duplicates before touching in-memory state.
		{
			FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			const FString ShortAssetName = FPackageName::GetShortName(PackageName);
			const FSoftObjectPath DiskPath(PackageName + TEXT(".") + ShortAssetName);
			if (AR.Get().GetAssetByObjectPath(DiskPath).IsValid())
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("An asset already exists at '%s' on disk. Choose a different asset_path."), *AssetPath));
			}
		}

		if (FindObject<UPackage>(nullptr, *PackageName))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("An asset already exists at '%s'. Choose a different asset_path."), *AssetPath));
		}

		UPackage* Pkg = CreatePackage(*PackageName);
		if (!Pkg)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Failed to create package '%s'. Check the path is valid."), *PackageName));
		}

		const FName MontageName = FPackageName::GetShortFName(PackageName);

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "CreateAnimMontage", "MCP: Create Anim Montage"));

		UAnimMontage* Montage = NewObject<UAnimMontage>(Pkg, MontageName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Montage)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(TEXT("NewObject<UAnimMontage> returned null. Engine error."));
		}

		// Wire skeleton.
		Montage->SetSkeleton(Skeleton);

		// Set up the first slot track (already created by default ctor — slot[0] is "DefaultGroup.DefaultSlot").
		// The slot tracks array has one element by default; we just rename its SlotName.
		if (Montage->SlotAnimTracks.Num() == 0)
		{
			Montage->AddSlot(FName(*SlotName));
		}
		else
		{
			Montage->SlotAnimTracks[0].SlotName = FName(*SlotName);
		}

		// Add a single segment covering [EffectiveStartTime, EffectiveEndTime] of the source.
		// StartPos=0 anchors the segment to the start of the montage timeline.
		{
			FAnimSegment Segment;
			Segment.SetAnimReference(SourceSeq, true);
			Segment.AnimStartTime = EffectiveStartTime;
			Segment.AnimEndTime   = EffectiveEndTime;
			Segment.StartPos      = 0.f;
			Segment.AnimPlayRate  = 1.f;
			Segment.LoopingCount  = 1;
			Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);
		}

		// Set composite length to the cropped duration.
		Montage->SetCompositeLength(MontageLength);

		// Ensure a default composite section at time 0.
		if (Montage->CompositeSections.Num() == 0)
		{
			FCompositeSection DefaultSection;
			DefaultSection.SetTime(0.f);
			DefaultSection.SectionName = FName(TEXT("Default"));
			Montage->CompositeSections.Add(DefaultSection);
		}

		// Refresh runtime cache.
		Montage->RefreshCacheData();

		// Notify asset registry and mark dirty.
		FAssetRegistryModule::AssetCreated(Montage);
		Pkg->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("created"), true);
		Result->SetStringField(TEXT("asset_path"), Montage->GetPathName());
		Result->SetNumberField(TEXT("length"),     static_cast<double>(MontageLength));
		Result->SetStringField(TEXT("slot"),       SlotName);
		Result->SetNumberField(TEXT("start_time"), static_cast<double>(EffectiveStartTime));
		Result->SetNumberField(TEXT("end_time"),   static_cast<double>(EffectiveEndTime));
		if (bEndTimeClamped)
		{
			Result->SetStringField(TEXT("end_time_note"),
				TEXT("end_time was clamped to source animation length."));
		}
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ---------------------------------------------------------------------------
	// add_anim_notify
	// ---------------------------------------------------------------------------

	FAgentMcpToolResult HandleAddAnimNotify(const TSharedPtr<FJsonObject>& Args)
	{
		FString AnimPath, NotifyClassToken;

		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AnimPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path'."));
		}
		if (!Args->TryGetStringField(TEXT("notify_class"), NotifyClassToken))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'notify_class'."));
		}

		// Resolve time. Exactly one of 'time' or 'fraction' must be supplied.
		double TimeValue = -1.0;
		double FractionValue = -1.0;
		const bool bHasTime = Args->TryGetNumberField(TEXT("time"), TimeValue);
		const bool bHasFraction = Args->TryGetNumberField(TEXT("fraction"), FractionValue);
		if (!bHasTime && !bHasFraction)
		{
			return FAgentMcpToolResult::Error(
				TEXT("Either 'time' (seconds) or 'fraction' (0-1) must be provided."));
		}
		if (bHasTime && bHasFraction)
		{
			return FAgentMcpToolResult::Error(
				TEXT("Provide only one of 'time' or 'fraction', not both."));
		}

		// Load anim asset.
		FString LoadError;
		UAnimSequenceBase* AnimAsset = ResolveAnimAsset(AnimPath, LoadError);
		if (!AnimAsset)
		{
			return FAgentMcpToolResult::Error(LoadError);
		}

		const float TotalLength = AnimAsset->GetPlayLength();
		float NotifyTime = 0.f;
		if (bHasFraction)
		{
			if (FractionValue < 0.0 || FractionValue > 1.0)
			{
				return FAgentMcpToolResult::Error(
					TEXT("'fraction' must be in the range [0, 1]."));
			}
			NotifyTime = static_cast<float>(FractionValue) * TotalLength;
		}
		else
		{
			NotifyTime = static_cast<float>(TimeValue);
			if (NotifyTime < 0.f || NotifyTime > TotalLength + UE_KINDA_SMALL_NUMBER)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("'time' %.3f is outside [0, %.3f] (asset length). Use 'fraction' instead if needed."),
					NotifyTime, TotalLength));
			}
			NotifyTime = FMath::Clamp(NotifyTime, 0.f, TotalLength);
		}

		// Resolve notify class.
		FString ClassError;
		UClass* NotifyClass = ResolveAnimNotifyClass(NotifyClassToken, ClassError);
		if (!NotifyClass)
		{
			return FAgentMcpToolResult::Error(ClassError);
		}

		// Optional properties object.
		const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
		Args->TryGetObjectField(TEXT("properties"), PropertiesObj);

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddAnimNotify", "MCP: Add Anim Notify"));
		AnimAsset->Modify();

		// Ensure at least one notify track exists.
		if (AnimAsset->AnimNotifyTracks.Num() == 0)
		{
			FAnimNotifyTrack NewTrack(TEXT("Notify"), FLinearColor::White);
			AnimAsset->AnimNotifyTracks.Add(NewTrack);
		}
		const int32 TrackIdx = 0;

		// Create the notify instance.
		UAnimNotify* NotifyInstance = NewObject<UAnimNotify>(AnimAsset, NotifyClass, NAME_None, RF_Transactional);
		if (!NotifyInstance)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Failed to create notify instance of class '%s'."), *NotifyClassToken));
		}

		// Build and fill the FAnimNotifyEvent.
		FAnimNotifyEvent NewEvent;
		NewEvent.Notify = NotifyInstance;
		NewEvent.NotifyStateClass = nullptr;
		NewEvent.TrackIndex = TrackIdx;

		// Link to asset at the correct time (sets internal segment/slot data).
		NewEvent.Link(AnimAsset, NotifyTime, 0);

		// Compute trigger-time offset (quantises to the nearest frame boundary).
		NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(
			AnimAsset->CalculateOffsetForNotify(NotifyTime));

		// Name: use the class-provided notify name, falling back to the class short name.
		NewEvent.NotifyName = FName(*NotifyInstance->GetNotifyName());

		// Guid required — SAnimNotifyPanel purges events with !Guid.IsValid() on next Persona refresh.
		NewEvent.Guid = FGuid::NewGuid();

		// Add to the Notifies array and register in the track's pointer list.
		AnimAsset->Notifies.Add(NewEvent);
		// Re-acquire pointer after Add (may reallocate).
		FAnimNotifyEvent& AddedEvent = AnimAsset->Notifies.Last();
		AnimAsset->AnimNotifyTracks[TrackIdx].Notifies.Add(&AddedEvent);

		// Fire editor-init hook so the notify can set up default state (e.g. populate properties).
		if (AddedEvent.Notify)
		{
			AddedEvent.Notify->OnAnimNotifyCreatedInEditor(AddedEvent);
		}

		// Apply optional properties via ImportText.
		TArray<FString> SetProps;
		TArray<FString> FailedProps;
		if (PropertiesObj && (*PropertiesObj).IsValid())
		{
			for (const auto& KV : (*PropertiesObj)->Values)
			{
				const FString PropName = KV.Key;
				const FString PropValue = KV.Value->AsString();
				FProperty* Prop = NotifyClass->FindPropertyByName(FName(*PropName));
				if (!Prop)
				{
					FailedProps.Add(PropName);
					continue;
				}
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NotifyInstance);
				const TCHAR* Result = Prop->ImportText_InContainer(*PropValue, NotifyInstance, NotifyInstance, PPF_None);
				if (Result)
				{
					SetProps.Add(PropName);
				}
				else
				{
					FailedProps.Add(PropName);
				}
			}
		}

		// Refresh the montage if applicable.
		if (UAnimMontage* Montage = Cast<UAnimMontage>(AnimAsset))
		{
			Montage->RefreshCacheData();
		}

		AnimAsset->PostEditChange();
		AnimAsset->MarkPackageDirty();

		// Build response.
		TArray<TSharedPtr<FJsonValue>> SetArr, FailArr;
		for (const FString& S : SetProps)  { SetArr.Add(MakeShared<FJsonValueString>(S)); }
		for (const FString& F : FailedProps) { FailArr.Add(MakeShared<FJsonValueString>(F)); }

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("notify_class"), NotifyClass->GetPathName());
		Result->SetNumberField(TEXT("time"), static_cast<double>(NotifyTime));
		Result->SetArrayField(TEXT("set_properties"), SetArr);
		Result->SetArrayField(TEXT("failed_properties"), FailArr);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ---------------------------------------------------------------------------
	// add_compatible_skeleton
	// ---------------------------------------------------------------------------

	/**
	 * Resolves a USkeleton from a package path.
	 * Returns nullptr and fills OutError on failure.
	 */
	USkeleton* ResolveSkeleton(const FString& Path, FString& OutError)
	{
		// Build a fully-qualified object path (PackageName.AssetName).
		// If the caller already supplied one (contains a dot) use it as-is.
		// Otherwise derive the asset name from the short package name.
		FString ObjectPath;
		{
			int32 DotIdx = INDEX_NONE;
			if (Path.FindChar(TEXT('.'), DotIdx))
			{
				ObjectPath = Path;
			}
			else
			{
				const FString ShortName = FPackageName::GetShortName(Path);
				ObjectPath = Path + TEXT(".") + ShortName;
			}
		}

		// FindObject covers transient / already-loaded objects (no disk I/O).
		USkeleton* Skel = FindObject<USkeleton>(nullptr, *ObjectPath);
		if (!Skel)
		{
			Skel = LoadObject<USkeleton>(nullptr, *ObjectPath);
		}
		if (!Skel)
		{
			OutError = FString::Printf(
				TEXT("Path '%s' could not be loaded as a USkeleton. "
				     "Verify the path points to a Skeleton asset (not a SkeletalMesh or other type)."),
				*Path);
		}
		return Skel;
	}

	FAgentMcpToolResult HandleAddCompatibleSkeleton(const TSharedPtr<FJsonObject>& Args)
	{
		FString SkeletonPath, CompatPath;

		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'skeleton_path'."));
		}
		if (!Args->TryGetStringField(TEXT("compatible_skeleton_path"), CompatPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'compatible_skeleton_path'."));
		}

		// Load both skeletons.
		FString ErrA, ErrB;
		USkeleton* MainSkeleton  = ResolveSkeleton(SkeletonPath, ErrA);
		if (!MainSkeleton)
		{
			return FAgentMcpToolResult::Error(ErrA);
		}
		USkeleton* CompatSkeleton = ResolveSkeleton(CompatPath, ErrB);
		if (!CompatSkeleton)
		{
			return FAgentMcpToolResult::Error(ErrB);
		}

		// Self-registration is not meaningful.
		if (MainSkeleton == CompatSkeleton)
		{
			return FAgentMcpToolResult::Error(
				TEXT("'skeleton_path' and 'compatible_skeleton_path' resolve to the same asset. "
				     "A skeleton cannot be added as compatible with itself."));
		}

		// Check idempotency: is the compat skeleton already registered?
		const TSoftObjectPtr<USkeleton> CompatSoft(CompatSkeleton);
		const TArray<TSoftObjectPtr<USkeleton>>& Existing = MainSkeleton->GetCompatibleSkeletons();
		for (const TSoftObjectPtr<USkeleton>& Entry : Existing)
		{
			if (Entry == CompatSoft)
			{
				// Already compatible — return idempotent success.
				TSharedRef<FJsonObject> IdempResult = MakeShared<FJsonObject>();
				IdempResult->SetBoolField(TEXT("added"),              false);
				IdempResult->SetBoolField(TEXT("already_compatible"), true);
				IdempResult->SetStringField(TEXT("skeleton"),            MainSkeleton->GetPathName());
				IdempResult->SetStringField(TEXT("compatible_skeleton"), CompatSkeleton->GetPathName());
				IdempResult->SetNumberField(TEXT("compatible_count"),
					static_cast<double>(Existing.Num()));
				return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(IdempResult));
			}
		}

		// Modify under a transaction.
		FScopedTransaction Transaction(
			NSLOCTEXT("AgentMcp", "AddCompatibleSkeleton", "MCP: Add Compatible Skeleton"));
		MainSkeleton->Modify();
		MainSkeleton->AddCompatibleSkeleton(CompatSkeleton);
		MainSkeleton->MarkPackageDirty();

		const int32 NewCount = MainSkeleton->GetCompatibleSkeletons().Num();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"),              true);
		Result->SetBoolField(TEXT("already_compatible"), false);
		Result->SetStringField(TEXT("skeleton"),            MainSkeleton->GetPathName());
		Result->SetStringField(TEXT("compatible_skeleton"), CompatSkeleton->GetPathName());
		Result->SetNumberField(TEXT("compatible_count"),    static_cast<double>(NewCount));
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AgentMcp::Tools::RegisterAnimMontageTools()
{
	// ── create_anim_montage ──────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("create_anim_montage");
		Def.Description = TEXT(
			"SafeWrite. Creates a UAnimMontage asset from an existing AnimSequence. "
			"The montage is skeleton-matched, contains one slot track, and a single segment. "
			"Optional start_time/end_time crop the segment to a sub-range of the source; omit both to use the full length. "
			"end_time exceeding the source length is silently clamped. "
			"Returns {created, asset_path, length, slot, start_time, end_time} (actual effective values). "
			"Args: source_animation (required), asset_path (required), slot_name (optional, default DefaultSlot), "
			"start_time (optional float seconds, default 0), end_time (optional float seconds, default source length).");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> SrcProp = MakeShared<FJsonObject>();
			SrcProp->SetStringField(TEXT("type"), TEXT("string"));
			SrcProp->SetStringField(TEXT("description"), TEXT("Package path of the source AnimSequence, e.g. /Game/Animations/AS_Run."));
			Props->SetObjectField(TEXT("source_animation"), SrcProp);

			TSharedRef<FJsonObject> DstProp = MakeShared<FJsonObject>();
			DstProp->SetStringField(TEXT("type"), TEXT("string"));
			DstProp->SetStringField(TEXT("description"), TEXT("Package path for the new AnimMontage, e.g. /Game/AbilitySystem/AM_Run."));
			Props->SetObjectField(TEXT("asset_path"), DstProp);

			TSharedRef<FJsonObject> SlotProp = MakeShared<FJsonObject>();
			SlotProp->SetStringField(TEXT("type"), TEXT("string"));
			SlotProp->SetStringField(TEXT("description"), TEXT("Slot name for the track (default DefaultSlot)."));
			Props->SetObjectField(TEXT("slot_name"), SlotProp);

			TSharedRef<FJsonObject> StartTimeProp = MakeShared<FJsonObject>();
			StartTimeProp->SetStringField(TEXT("type"), TEXT("number"));
			StartTimeProp->SetStringField(TEXT("description"),
				TEXT("Start time in seconds within the source animation (default 0). Must be >= 0."));
			Props->SetObjectField(TEXT("start_time"), StartTimeProp);

			TSharedRef<FJsonObject> EndTimeProp = MakeShared<FJsonObject>();
			EndTimeProp->SetStringField(TEXT("type"), TEXT("number"));
			EndTimeProp->SetStringField(TEXT("description"),
				TEXT("End time in seconds within the source animation (default = full source length). "
				     "Values exceeding the source length are clamped. Must be > start_time."));
			Props->SetObjectField(TEXT("end_time"), EndTimeProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("source_animation")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleCreateAnimMontage);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── add_anim_notify ──────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_anim_notify");
		Def.Description = TEXT(
			"SafeWrite. Adds a UAnimNotify to a UAnimSequenceBase (montage or sequence). "
			"Exactly one of 'time' (seconds) or 'fraction' (0-1) must be provided. "
			"Optional 'properties' object applies FProperty::ImportText to the notify instance. "
			"Returns {added, notify_class, time, set_properties, failed_properties}. "
			"Args: asset_path (required), notify_class (required, e.g. /Script/Engine.AnimNotify_PlaySound), time (seconds) or fraction (0-1), properties (optional object).");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> AssetProp = MakeShared<FJsonObject>();
			AssetProp->SetStringField(TEXT("type"), TEXT("string"));
			AssetProp->SetStringField(TEXT("description"), TEXT("Package path of the AnimSequenceBase (montage or sequence) to add the notify to."));
			Props->SetObjectField(TEXT("asset_path"), AssetProp);

			TSharedRef<FJsonObject> ClassProp = MakeShared<FJsonObject>();
			ClassProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassProp->SetStringField(TEXT("description"), TEXT("UAnimNotify subclass path or short name, e.g. /Script/Engine.AnimNotify_PlaySound."));
			Props->SetObjectField(TEXT("notify_class"), ClassProp);

			TSharedRef<FJsonObject> TimeProp = MakeShared<FJsonObject>();
			TimeProp->SetStringField(TEXT("type"), TEXT("number"));
			TimeProp->SetStringField(TEXT("description"), TEXT("Absolute time in seconds to place the notify. Mutually exclusive with 'fraction'."));
			Props->SetObjectField(TEXT("time"), TimeProp);

			TSharedRef<FJsonObject> FractionProp = MakeShared<FJsonObject>();
			FractionProp->SetStringField(TEXT("type"), TEXT("number"));
			FractionProp->SetStringField(TEXT("description"), TEXT("Fractional position 0-1 (0=start, 1=end). Mutually exclusive with 'time'."));
			Props->SetObjectField(TEXT("fraction"), FractionProp);

			TSharedRef<FJsonObject> PropsPropSchema = MakeShared<FJsonObject>();
			PropsPropSchema->SetStringField(TEXT("type"), TEXT("object"));
			PropsPropSchema->SetStringField(TEXT("description"), TEXT("Optional key/value pairs to set on the notify instance via ImportText, e.g. {\"EventTag\":\"(TagName=\\\"Event.Montage.Hit\\\")\"}. Failed properties are listed in failed_properties."));
			Props->SetObjectField(TEXT("properties"), PropsPropSchema);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("notify_class")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddAnimNotify);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── add_compatible_skeleton ──────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_compatible_skeleton");
		Def.Description = TEXT(
			"SafeWrite. Registers a foreign USkeleton as compatible with the project's main skeleton, "
			"solving the common issue where marketplace animation packs ship their own USkeleton copy "
			"(structurally identical but a different asset) that prevents animations from playing on the "
			"project character. Wraps USkeleton::AddCompatibleSkeleton under a scoped transaction and "
			"marks the package dirty. "
			"Idempotent: if the skeleton is already listed as compatible, returns {added:false, already_compatible:true} without modifying the asset. "
			"Returns {added, already_compatible, skeleton, compatible_skeleton, compatible_count}. "
			"Args: skeleton_path (required, project main skeleton e.g. /Game/Characters/Mannequins/Meshes/SK_Mannequin), "
			"compatible_skeleton_path (required, the marketplace/foreign skeleton to register).");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> SkelProp = MakeShared<FJsonObject>();
			SkelProp->SetStringField(TEXT("type"), TEXT("string"));
			SkelProp->SetStringField(TEXT("description"),
				TEXT("Package path of the project's main USkeleton to modify, "
				     "e.g. /Game/Characters/Mannequins/Meshes/SK_Mannequin."));
			Props->SetObjectField(TEXT("skeleton_path"), SkelProp);

			TSharedRef<FJsonObject> CompatProp = MakeShared<FJsonObject>();
			CompatProp->SetStringField(TEXT("type"), TEXT("string"));
			CompatProp->SetStringField(TEXT("description"),
				TEXT("Package path of the foreign USkeleton to register as compatible, "
				     "e.g. /Game/MarketplacePack/Characters/SK_Hero_Skeleton."));
			Props->SetObjectField(TEXT("compatible_skeleton_path"), CompatProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("skeleton_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("compatible_skeleton_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddCompatibleSkeleton);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
