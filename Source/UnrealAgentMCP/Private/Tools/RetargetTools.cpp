#include "Tools/RetargetTools.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationBlueprintLibrary.h"
#include "AnimPose.h"
#include "AssetRegistry/AssetData.h"
#include "AssetToolsModule.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Tools/McpToolUtils.h"
#include "UObject/Package.h"

namespace
{
	using namespace AgentMcp;

	/** Loads an asset of type T from a /Game package path; fills OutError when missing. */
	template <typename T>
	T* LoadGameAsset(const FString& Path, const TCHAR* What, FString& OutError)
	{
		FString PackageName = Path;
		int32 DotIdx = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIdx))
		{
			PackageName = PackageName.Left(DotIdx);
		}
		const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		T* Asset = LoadObject<T>(nullptr, *ObjectPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("%s not found or wrong type: '%s'."), What, *Path);
		}
		return Asset;
	}

	/** Applies chain remove/override entries from JSON onto one IK rig controller. */
	bool ApplyChainEdits(UIKRigController* Controller, const TSharedPtr<FJsonObject>& Args,
		const TCHAR* RemoveField, const TCHAR* ChainField, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Removals = nullptr;
		if (Args->TryGetArrayField(RemoveField, Removals))
		{
			for (const TSharedPtr<FJsonValue>& V : *Removals)
			{
				Controller->RemoveRetargetChain(FName(*V->AsString()));
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Chains = nullptr;
		if (Args->TryGetArrayField(ChainField, Chains))
		{
			for (const TSharedPtr<FJsonValue>& V : *Chains)
			{
				const TSharedPtr<FJsonObject> Obj = V->AsObject();
				FString Name, Start, End;
				if (!Obj.IsValid() ||
					!Obj->TryGetStringField(TEXT("name"), Name) ||
					!Obj->TryGetStringField(TEXT("start_bone"), Start) ||
					!Obj->TryGetStringField(TEXT("end_bone"), End))
				{
					OutError = FString::Printf(
						TEXT("Each '%s' entry requires name/start_bone/end_bone strings."), ChainField);
					return false;
				}
				Controller->RemoveRetargetChain(FName(*Name)); // replace-if-exists
				Controller->AddRetargetChain(FName(*Name), FName(*Start), FName(*End), NAME_None);
			}
		}
		return true;
	}

	/**
	 * Builds an exact retarget pose on the target side from a reference animation's frame 0
	 * (e.g. the project's MM_T_Pose): per-bone local-space delta vs the mesh reference pose.
	 * Restricting via BoneFilter avoids baking pose-carry garbage into unmapped bones —
	 * the batch retargeter writes tracks for EVERY target bone.
	 */
	int32 BuildRetargetPoseFromAnimation(UIKRetargeterController* RC, UAnimSequence* PoseAnim,
		const TSet<FName>& BoneFilter)
	{
		const FName PoseName(TEXT("McpPose_FromAnimation"));
		RC->CreateRetargetPose(PoseName, ERetargetSourceOrTarget::Target);
		RC->SetCurrentRetargetPose(PoseName, ERetargetSourceOrTarget::Target);

		FAnimPose Pose;
		FAnimPoseEvaluationOptions Options;
		UAnimPoseExtensions::GetAnimPoseAtTime(PoseAnim, 0.0, Options, Pose);
		TArray<FName> Bones;
		UAnimPoseExtensions::GetBoneNames(Pose, Bones);

		int32 Applied = 0;
		for (const FName& Bone : Bones)
		{
			if (BoneFilter.Num() > 0 && !BoneFilter.Contains(Bone))
			{
				continue;
			}
			const FQuat Local = UAnimPoseExtensions::GetBonePose(Pose, Bone, EAnimPoseSpaces::Local).GetRotation();
			const FQuat Ref = UAnimPoseExtensions::GetRefBonePose(Pose, Bone, EAnimPoseSpaces::Local).GetRotation();
			const FQuat Delta = Ref.Inverse() * Local;
			if (Delta.GetAngle() > FMath::DegreesToRadians(0.5f))
			{
				RC->SetRotationOffsetForRetargetPoseBone(Bone, Delta, ERetargetSourceOrTarget::Target);
				++Applied;
			}
		}
		return Applied;
	}

	FAgentMcpToolResult HandleRetargetAnimations(const TSharedPtr<FJsonObject>& Args)
	{
		// ── Required args ────────────────────────────────────────────────────
		FString SourceMeshPath, TargetMeshPath, OutputPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("source_mesh"), SourceMeshPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'source_mesh'."));
		}
		if (!Args->TryGetStringField(TEXT("target_mesh"), TargetMeshPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'target_mesh'."));
		}
		if (!Args->TryGetStringField(TEXT("output_path"), OutputPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'output_path'."));
		}
		OutputPath.RemoveFromEnd(TEXT("/"));
		if (OutputPath != TEXT("/Game") && !OutputPath.StartsWith(TEXT("/Game/")))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'output_path' must be under /Game. Got: '%s'."), *OutputPath));
		}
		const TArray<TSharedPtr<FJsonValue>>* AnimsJson = nullptr;
		if (!Args->TryGetArrayField(TEXT("animations"), AnimsJson) || AnimsJson->Num() == 0)
		{
			return FAgentMcpToolResult::Error(TEXT("'animations' must be a non-empty array of AnimSequence paths."));
		}

		FString Suffix = TEXT("_Retargeted");
		Args->TryGetStringField(TEXT("suffix"), Suffix);
		bool bSave = true;
		Args->TryGetBoolField(TEXT("save"), bSave);

		// ── Load assets ──────────────────────────────────────────────────────
		FString LoadError;
		USkeletalMesh* SourceMesh = LoadGameAsset<USkeletalMesh>(SourceMeshPath, TEXT("source_mesh"), LoadError);
		if (!SourceMesh) { return FAgentMcpToolResult::Error(LoadError); }
		USkeletalMesh* TargetMesh = LoadGameAsset<USkeletalMesh>(TargetMeshPath, TEXT("target_mesh"), LoadError);
		if (!TargetMesh) { return FAgentMcpToolResult::Error(LoadError); }

		TArray<UAnimSequence*> Sources;
		TArray<FAssetData> SourceAssetData;
		for (const TSharedPtr<FJsonValue>& V : *AnimsJson)
		{
			UAnimSequence* Seq = LoadGameAsset<UAnimSequence>(V->AsString(), TEXT("animations entry"), LoadError);
			if (!Seq) { return FAgentMcpToolResult::Error(LoadError); }
			Sources.Add(Seq);
			SourceAssetData.Add(FAssetData(Seq));
		}

		// ── Rig + retargeter setup (reused when the retargeter already exists) ──
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		const FString RetargeterName = FString::Printf(TEXT("RTG_%s_to_%s"), *SourceMesh->GetName(), *TargetMesh->GetName());
		const FString RetargeterPath = OutputPath + TEXT("/") + RetargeterName;

		TArray<FString> Warnings;
		UIKRetargeter* Retargeter = LoadObject<UIKRetargeter>(nullptr, *(RetargeterPath + TEXT(".") + RetargeterName));
		bool bReused = Retargeter != nullptr;

		if (!bReused)
		{
			auto MakeAutoRig = [&](USkeletalMesh* Mesh, const FString& Name, const FString& RootOverride) -> UIKRigDefinition*
			{
				UIKRigDefinition* Rig = Cast<UIKRigDefinition>(AssetTools.CreateAsset(
					Name, OutputPath, UIKRigDefinition::StaticClass(), NewObject<UIKRigDefinitionFactory>()));
				if (!Rig) { return nullptr; }
				UIKRigController* C = UIKRigController::GetController(Rig);
				C->SetSkeletalMesh(Mesh);
				C->ApplyAutoGeneratedRetargetDefinition();
				if (!RootOverride.IsEmpty())
				{
					C->SetRetargetRoot(FName(*RootOverride));
				}
				if (C->GetRetargetRoot().IsNone())
				{
					Warnings.Add(FString::Printf(
						TEXT("Auto-characterizer found no retarget root on '%s' — pass retarget_root_source/_target."), *Mesh->GetName()));
				}
				return Rig;
			};

			FString RootSrc, RootTgt;
			Args->TryGetStringField(TEXT("retarget_root_source"), RootSrc);
			Args->TryGetStringField(TEXT("retarget_root_target"), RootTgt);

			UIKRigDefinition* SrcRig = MakeAutoRig(SourceMesh, FString::Printf(TEXT("IK_%s_AutoSrc"), *SourceMesh->GetName()), RootSrc);
			UIKRigDefinition* TgtRig = MakeAutoRig(TargetMesh, FString::Printf(TEXT("IK_%s_AutoTgt"), *TargetMesh->GetName()), RootTgt);
			if (!SrcRig || !TgtRig)
			{
				return FAgentMcpToolResult::Error(TEXT("Failed to create IK Rig assets (see Output Log)."));
			}

			FString ChainError;
			if (!ApplyChainEdits(UIKRigController::GetController(SrcRig), Args, TEXT("remove_source_chains"), TEXT("source_chains"), ChainError) ||
				!ApplyChainEdits(UIKRigController::GetController(TgtRig), Args, TEXT("remove_target_chains"), TEXT("target_chains"), ChainError))
			{
				return FAgentMcpToolResult::Error(ChainError);
			}

			Retargeter = Cast<UIKRetargeter>(AssetTools.CreateAsset(
				RetargeterName, OutputPath, UIKRetargeter::StaticClass(), NewObject<UIKRetargetFactory>()));
			if (!Retargeter)
			{
				return FAgentMcpToolResult::Error(TEXT("Failed to create IKRetargeter asset."));
			}
			UIKRetargeterController* RC = UIKRetargeterController::GetController(Retargeter);
			RC->SetIKRig(ERetargetSourceOrTarget::Source, SrcRig);
			RC->SetIKRig(ERetargetSourceOrTarget::Target, TgtRig);
			RC->AutoMapChains(EAutoMapChainType::Fuzzy, true);

			// Optional exact retarget pose from a reference animation (e.g. a T-pose clip).
			FString TPoseAnimPath;
			if (Args->TryGetStringField(TEXT("target_tpose_animation"), TPoseAnimPath) && !TPoseAnimPath.IsEmpty())
			{
				UAnimSequence* TPoseAnim = LoadGameAsset<UAnimSequence>(TPoseAnimPath, TEXT("target_tpose_animation"), LoadError);
				if (!TPoseAnim) { return FAgentMcpToolResult::Error(LoadError); }
				TSet<FName> BoneFilter;
				const TArray<TSharedPtr<FJsonValue>>* TPoseBones = nullptr;
				if (Args->TryGetArrayField(TEXT("tpose_bones"), TPoseBones))
				{
					for (const TSharedPtr<FJsonValue>& V : *TPoseBones)
					{
						BoneFilter.Add(FName(*V->AsString()));
					}
				}
				BuildRetargetPoseFromAnimation(RC, TPoseAnim, BoneFilter);
			}
		}
		else
		{
			Warnings.Add(TEXT("Existing retargeter reused — chain/pose arguments ignored. Delete it to rebuild from scratch."));
		}

		// ── Batch retarget ───────────────────────────────────────────────────
		UIKRetargetBatchOperation* Batch = NewObject<UIKRetargetBatchOperation>();
		const TArray<FAssetData> NewAssets = Batch->DuplicateAndRetarget(
			SourceAssetData, SourceMesh, TargetMesh, Retargeter,
			FString(), FString(), FString(), Suffix, /*bIncludeReferencedAssets=*/false);
		if (NewAssets.Num() == 0)
		{
			return FAgentMcpToolResult::Error(
				TEXT("Batch retarget produced no assets — check that both rigs have mapped chains and a retarget root (see Output Log)."));
		}

		// ── Post-process: move outputs to output_path, strip unwanted tracks ──
		TArray<FString> StripPrefixes;
		{
			const TArray<TSharedPtr<FJsonValue>>* PrefixesJson = nullptr;
			if (Args->TryGetArrayField(TEXT("strip_track_prefixes"), PrefixesJson))
			{
				for (const TSharedPtr<FJsonValue>& V : *PrefixesJson)
				{
					StripPrefixes.Add(V->AsString());
				}
			}
		}

		UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		int32 StrippedTracks = 0;
		TArray<TSharedPtr<FJsonValue>> OutputArray;
		for (int32 Index = 0; Index < NewAssets.Num(); ++Index)
		{
			UAnimSequence* OutSeq = Cast<UAnimSequence>(NewAssets[Index].GetAsset());
			if (!OutSeq)
			{
				continue;
			}

			// The batch op drops outputs wherever it pleases (typically /Game root) — relocate.
			FString FinalPath = OutSeq->GetPackage()->GetName();
			const FString WantedPath = OutputPath + TEXT("/") + OutSeq->GetName();
			if (AssetSubsystem && FinalPath != WantedPath)
			{
				if (AssetSubsystem->RenameAsset(OutSeq->GetPathName(), WantedPath))
				{
					FinalPath = WantedPath;
				}
				else
				{
					Warnings.Add(FString::Printf(TEXT("Could not move '%s' to '%s'."), *OutSeq->GetName(), *OutputPath));
				}
			}

			if (StripPrefixes.Num() > 0)
			{
				TArray<FName> TrackNames;
				UAnimationBlueprintLibrary::GetAnimationTrackNames(OutSeq, TrackNames);
				for (const FName& Track : TrackNames)
				{
					const FString TrackStr = Track.ToString();
					for (const FString& Prefix : StripPrefixes)
					{
						if (TrackStr.StartsWith(Prefix))
						{
							UAnimationBlueprintLibrary::RemoveBoneAnimation(OutSeq, Track);
							++StrippedTracks;
							break;
						}
					}
				}
			}

			if (bSave && AssetSubsystem)
			{
				AssetSubsystem->SaveAsset(FinalPath, /*bOnlyIfIsDirty=*/false);
			}

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("source"), Sources.IsValidIndex(Index) ? Sources[Index]->GetPathName() : TEXT(""));
			Entry->SetStringField(TEXT("output"), FinalPath);
			OutputArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		if (bSave && AssetSubsystem && !bReused)
		{
			AssetSubsystem->SaveDirectory(OutputPath, /*bOnlyIfIsDirty=*/true, /*bRecursive=*/false);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("retargeted"), true);
		Result->SetStringField(TEXT("retargeter"), RetargeterPath);
		Result->SetBoolField(TEXT("reused_retargeter"), bReused);
		Result->SetNumberField(TEXT("stripped_tracks"), StrippedTracks);
		Result->SetArrayField(TEXT("outputs"), OutputArray);
		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningArray;
			for (const FString& W : Warnings)
			{
				WarningArray.Add(MakeShared<FJsonValueString>(W));
			}
			Result->SetArrayField(TEXT("warnings"), WarningArray);
		}
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}
} // namespace

void AgentMcp::Tools::RegisterRetargetTools()
{
	// ── retarget_animations ──────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("retarget_animations");
		Def.Description = TEXT(
			"SafeWrite. Batch-retargets AnimSequences from one skeletal mesh to another via auto-characterized IK Rigs + IK Retargeter. "
			"Hard-won defaults baked in: pass anatomically CORRESPONDENT chains via source_chains/target_chains (per-index FK transfer — e.g. for a rig "
			"without clavicles, end the source arm at the wrist and start the target arm at the upperarm, and remove the target's standalone clavicle chains); "
			"target_tpose_animation builds an exact retarget pose from a T-pose clip's frame 0 (restrict via tpose_bones); strip_track_prefixes removes "
			"baked tracks for unmapped bones (the batch op writes tracks for EVERY target bone — fingers become garbage otherwise). "
			"Creates IK_<mesh>_AutoSrc/_AutoTgt + RTG_<src>_to_<tgt> under output_path; an existing retargeter is REUSED (edit it in-editor, rerun to apply). "
			"Returns {retargeted, retargeter, reused_retargeter, stripped_tracks, outputs:[{source, output}], warnings?}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			auto AddProp = [&Props](const TCHAR* Name, const TCHAR* Type, const TCHAR* Desc)
			{
				TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("type"), Type);
				P->SetStringField(TEXT("description"), Desc);
				Props->SetObjectField(Name, P);
			};
			AddProp(TEXT("source_mesh"), TEXT("string"), TEXT("Package path of the source SkeletalMesh (the rig the animations are authored on)."));
			AddProp(TEXT("target_mesh"), TEXT("string"), TEXT("Package path of the target SkeletalMesh to retarget onto."));
			AddProp(TEXT("animations"), TEXT("array"), TEXT("AnimSequence package paths to retarget (non-empty)."));
			AddProp(TEXT("output_path"), TEXT("string"), TEXT("Destination folder under /Game for outputs, rigs and the retargeter."));
			AddProp(TEXT("suffix"), TEXT("string"), TEXT("Appended to each output asset name (default _Retargeted)."));
			AddProp(TEXT("retarget_root_source"), TEXT("string"), TEXT("Pelvis bone override for the source rig when auto-characterization misses it (e.g. 'hips')."));
			AddProp(TEXT("retarget_root_target"), TEXT("string"), TEXT("Pelvis bone override for the target rig."));
			AddProp(TEXT("source_chains"), TEXT("array"), TEXT("Chain overrides [{name,start_bone,end_bone}] applied to the source rig after auto-characterization (replace-if-exists)."));
			AddProp(TEXT("target_chains"), TEXT("array"), TEXT("Chain overrides for the target rig."));
			AddProp(TEXT("remove_source_chains"), TEXT("array"), TEXT("Chain names to delete from the source rig."));
			AddProp(TEXT("remove_target_chains"), TEXT("array"), TEXT("Chain names to delete from the target rig (e.g. standalone clavicle chains when the source has none)."));
			AddProp(TEXT("target_tpose_animation"), TEXT("string"), TEXT("AnimSequence whose frame 0 defines the target retarget pose (use when source ref pose is T and target is A)."));
			AddProp(TEXT("tpose_bones"), TEXT("array"), TEXT("Restrict the T-pose offsets to these bones (recommended: the arm chain bones only)."));
			AddProp(TEXT("strip_track_prefixes"), TEXT("array"), TEXT("Bone-name prefixes whose tracks are removed from outputs (e.g. thumb_, index_ ... for finger-less source rigs)."));
			AddProp(TEXT("save"), TEXT("boolean"), TEXT("Write outputs/rigs to disk (default true)."));
			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("source_mesh")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("target_mesh")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("animations")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("output_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleRetargetAnimations);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
