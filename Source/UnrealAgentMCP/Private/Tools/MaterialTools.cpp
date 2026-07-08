#include "Tools/MaterialTools.h"

#include "AssetToolsModule.h"
#include "Components/MeshComponent.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "SceneTypes.h"
#include "Tools/McpToolUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

// M1 material authoring family — wraps UMaterialEditingLibrary (MaterialEditor module).
// describe_material and the round-trip assertions read the material asset directly
// (GetExpressionInputForProperty / UMaterialExpression::GetInput) rather than the library's
// "active material editor" accessors, so introspection works headless with no editor tab open.

namespace
{
	using namespace AgentMcp;

	// ── ImportText error sink (mirrors AnimGraphTools' FAnimImportErrors) ─────────
	class FMatImportErrors final : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			if (!Captured.IsEmpty()) { Captured += TEXT("; "); }
			Captured += V;
		}
	};

	// ── material output properties: name ↔ EMaterialProperty (connect + describe) ──
	struct FMatPropEntry { const TCHAR* Name; EMaterialProperty Prop; };
	static const FMatPropEntry GMatProps[] = {
		{ TEXT("BaseColor"),           MP_BaseColor },
		{ TEXT("Metallic"),            MP_Metallic },
		{ TEXT("Specular"),            MP_Specular },
		{ TEXT("Roughness"),           MP_Roughness },
		{ TEXT("Anisotropy"),          MP_Anisotropy },
		{ TEXT("EmissiveColor"),       MP_EmissiveColor },
		{ TEXT("Opacity"),             MP_Opacity },
		{ TEXT("OpacityMask"),         MP_OpacityMask },
		{ TEXT("Normal"),              MP_Normal },
		{ TEXT("Tangent"),             MP_Tangent },
		{ TEXT("WorldPositionOffset"), MP_WorldPositionOffset },
		{ TEXT("SubsurfaceColor"),     MP_SubsurfaceColor },
		{ TEXT("AmbientOcclusion"),    MP_AmbientOcclusion },
		{ TEXT("Refraction"),          MP_Refraction },
		{ TEXT("PixelDepthOffset"),    MP_PixelDepthOffset },
	};

	bool ResolveMaterialProperty(const FString& Name, EMaterialProperty& Out, FString& OutError)
	{
		for (const FMatPropEntry& E : GMatProps)
		{
			if (Name.Equals(E.Name, ESearchCase::IgnoreCase)) { Out = E.Prop; return true; }
		}
		OutError = FString::Printf(
			TEXT("Unknown material property '%s'. Use BaseColor/Metallic/Specular/Roughness/EmissiveColor/Opacity/OpacityMask/Normal/WorldPositionOffset/AmbientOcclusion/…."),
			*Name);
		return false;
	}

	// ── asset resolution / class resolution / node lookup ────────────────────────
	UMaterial* ResolveMaterial(const FString& Path, FString& OutError)
	{
		UMaterial* Mat = FindObject<UMaterial>(nullptr, *Path);
		if (!Mat)
		{
			FString Pkg = Path;
			int32 Dot = INDEX_NONE;
			if (Pkg.FindChar(TEXT('.'), Dot)) { Pkg = Pkg.Left(Dot); }
			const FString ObjPath = Pkg + TEXT(".") + FPackageName::GetShortName(Pkg);
			Mat = FindObject<UMaterial>(nullptr, *ObjPath);
			if (!Mat) { Mat = LoadObject<UMaterial>(nullptr, *ObjPath); }
		}
		if (!Mat)
		{
			OutError = FString::Printf(
				TEXT("Material not found: '%s'. Use search_assets with class filter Material to discover it."), *Path);
		}
		return Mat;
	}

	UClass* ResolveExpressionClass(const FString& ClassName, FString& OutError)
	{
		UClass* C = nullptr;
		if (ClassName.Contains(TEXT(".")))
		{
			// Full object path, e.g. /Script/Engine.MaterialExpressionConstant3Vector.
			C = FindObject<UClass>(nullptr, *ClassName);
			if (!C) { C = LoadObject<UClass>(nullptr, *ClassName); }
		}
		else
		{
			// Short name → built-in engine expression path. Resolving via the full
			// /Script/Engine path avoids the "short type name provided for TryFindType"
			// warning+callstack that TryFindTypeSlow emits for un-pathed names.
			const FString Normalized = ClassName.StartsWith(TEXT("MaterialExpression"))
				? ClassName : TEXT("MaterialExpression") + ClassName;
			const FString EnginePath = TEXT("/Script/Engine.") + Normalized;
			C = FindObject<UClass>(nullptr, *EnginePath);
			if (!C) { C = LoadObject<UClass>(nullptr, *EnginePath); }
			// Last resort for expressions defined outside /Script/Engine (rare; may warn).
			if (!C) { C = UClass::TryFindTypeSlow<UClass>(Normalized); }
		}
		if (!C || !C->IsChildOf(UMaterialExpression::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a UMaterialExpression subclass. Use e.g. Constant3Vector, ScalarParameter, TextureSample, Multiply, Add, TextureCoordinate."),
				*ClassName);
			return nullptr;
		}
		return C;
	}

	UMaterialExpression* FindExpressionById(UMaterial* Mat, const FString& Id)
	{
		for (UMaterialExpression* E : Mat->GetExpressions())
		{
			if (E && E->GetName() == Id) { return E; }
		}
		return nullptr;
	}

	// Resolves a Material OR MaterialInstance (both UMaterialInterface) — for MIC parents / assign targets.
	UMaterialInterface* ResolveMaterialInterface(const FString& Path, FString& OutError)
	{
		UMaterialInterface* MI = FindObject<UMaterialInterface>(nullptr, *Path);
		if (!MI)
		{
			FString Pkg = Path;
			int32 Dot = INDEX_NONE;
			if (Pkg.FindChar(TEXT('.'), Dot)) { Pkg = Pkg.Left(Dot); }
			const FString ObjPath = Pkg + TEXT(".") + FPackageName::GetShortName(Pkg);
			MI = FindObject<UMaterialInterface>(nullptr, *ObjPath);
			if (!MI) { MI = LoadObject<UMaterialInterface>(nullptr, *ObjPath); }
		}
		if (!MI) { OutError = FString::Printf(TEXT("Material/MaterialInstance not found: '%s'."), *Path); }
		return MI;
	}

	UMaterialInstanceConstant* ResolveMaterialInstance(const FString& Path, FString& OutError)
	{
		UMaterialInstanceConstant* MIC = FindObject<UMaterialInstanceConstant>(nullptr, *Path);
		if (!MIC)
		{
			FString Pkg = Path;
			int32 Dot = INDEX_NONE;
			if (Pkg.FindChar(TEXT('.'), Dot)) { Pkg = Pkg.Left(Dot); }
			const FString ObjPath = Pkg + TEXT(".") + FPackageName::GetShortName(Pkg);
			MIC = FindObject<UMaterialInstanceConstant>(nullptr, *ObjPath);
			if (!MIC) { MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjPath); }
		}
		if (!MIC) { OutError = FString::Printf(TEXT("MaterialInstanceConstant not found: '%s'. Create one with create_material_instance."), *Path); }
		return MIC;
	}

	// Schema sugar (TypedProp/MakeSchema/RegisterOne) lives in Tools/McpToolUtils.h —
	// shared namespace-qualified inlines, not per-file anonymous copies (P6 unity C2084).

	// ── handlers ─────────────────────────────────────────────────────────────────
	FAgentMcpToolResult HandleCreateMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString Name, Dest;
		if (!Args->TryGetStringField(TEXT("name"), Name))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'name'.")); }
		if (!Args->TryGetStringField(TEXT("destination_path"), Dest))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'destination_path'.")); }
		if (!Dest.StartsWith(TEXT("/Game")))
			{ return FAgentMcpToolResult::Error(TEXT("destination_path must be under /Game.")); }

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UObject* NewAsset = AssetTools.CreateAsset(Name, Dest, UMaterial::StaticClass(), Factory);
		UMaterial* NewMat = Cast<UMaterial>(NewAsset);
		if (!NewMat)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("CreateAsset failed for '%s' in '%s' (name taken or invalid path?)."), *Name, *Dest)); }

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("object_path"), NewMat->GetPathName());
		Out->SetStringField(TEXT("package_path"), Dest / Name);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleAddMaterialExpression(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MatPath, ClassName;
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		if (!Args->TryGetStringField(TEXT("expression_class"), ClassName))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'expression_class'.")); }

		FString Err;
		UMaterial* Mat = ResolveMaterial(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }
		UClass* ExprClass = ResolveExpressionClass(ClassName, Err);
		if (!ExprClass) { return FAgentMcpToolResult::Error(Err); }

		double PosX = 0.0, PosY = 0.0;
		Args->TryGetNumberField(TEXT("pos_x"), PosX);
		Args->TryGetNumberField(TEXT("pos_y"), PosY);

		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
			Mat, ExprClass, static_cast<int32>(PosX), static_cast<int32>(PosY));
		if (!Expr) { return FAgentMcpToolResult::Error(TEXT("CreateMaterialExpression returned null.")); }
		Mat->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("node_id"), Expr->GetName());
		Out->SetStringField(TEXT("object_path"), Expr->GetPathName());
		Out->SetStringField(TEXT("class"), ExprClass->GetName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleSetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MatPath, NodeId, PropName, Value;
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		if (!Args->TryGetStringField(TEXT("node_id"), NodeId))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'node_id'.")); }
		if (!Args->TryGetStringField(TEXT("property"), PropName))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'property'.")); }
		if (!Args->TryGetStringField(TEXT("value"), Value))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'value'.")); }

		FString Err;
		UMaterial* Mat = ResolveMaterial(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }
		UMaterialExpression* Expr = FindExpressionById(Mat, NodeId);
		if (!Expr)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("No expression node '%s' in the material. Use describe_material."), *NodeId)); }

		FProperty* Prop = Expr->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Property '%s' not found on %s."), *PropName, *Expr->GetClass()->GetName())); }

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);
		FMatImportErrors ErrDev;
		const TCHAR* Remainder = Prop->ImportText_Direct(*Value, ValuePtr, Expr, PPF_None, &ErrDev);
		if (!Remainder || !ErrDev.Captured.IsEmpty())
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("ImportText failed for %s='%s': %s"), *PropName, *Value, *ErrDev.Captured)); }
		Mat->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleConnectMaterialExpression(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MatPath, FromId, ToId, FromOut, ToIn;
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		if (!Args->TryGetStringField(TEXT("from_node"), FromId))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'from_node'.")); }
		if (!Args->TryGetStringField(TEXT("to_node"), ToId))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'to_node'.")); }
		Args->TryGetStringField(TEXT("from_output"), FromOut);
		Args->TryGetStringField(TEXT("to_input"), ToIn);

		FString Err;
		UMaterial* Mat = ResolveMaterial(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }
		UMaterialExpression* From = FindExpressionById(Mat, FromId);
		UMaterialExpression* To = FindExpressionById(Mat, ToId);
		if (!From || !To)
			{ return FAgentMcpToolResult::Error(TEXT("from_node or to_node not found. Use describe_material for node ids.")); }

		const bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOut, To, ToIn);
		Mat->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("connected"), bConnected);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleConnectMaterialProperty(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MatPath, FromId, PropName, FromOut;
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		if (!Args->TryGetStringField(TEXT("from_node"), FromId))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'from_node'.")); }
		if (!Args->TryGetStringField(TEXT("property"), PropName))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'property'.")); }
		Args->TryGetStringField(TEXT("from_output"), FromOut);

		FString Err;
		UMaterial* Mat = ResolveMaterial(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }
		UMaterialExpression* From = FindExpressionById(Mat, FromId);
		if (!From)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("from_node '%s' not found."), *FromId)); }
		EMaterialProperty Prop;
		if (!ResolveMaterialProperty(PropName, Prop, Err)) { return FAgentMcpToolResult::Error(Err); }

		const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOut, Prop);
		Mat->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("connected"), bConnected);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleRecompileMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MatPath;
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		FString Err;
		UMaterial* Mat = ResolveMaterial(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }

		UMaterialEditingLibrary::LayoutMaterialExpressions(Mat);
		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Mat);
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("recompiled"), true);
		Out->SetNumberField(TEXT("pixel_shader_instructions"), Stats.NumPixelShaderInstructions);
		Out->SetNumberField(TEXT("samplers"), Stats.NumSamplers);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleDescribeMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MatPath;
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		FString Err;
		UMaterial* Mat = ResolveMaterial(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }

		// Nodes.
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Edges;
		for (UMaterialExpression* E : Mat->GetExpressions())
		{
			if (!E) { continue; }
			int32 X = 0, Y = 0;
			UMaterialEditingLibrary::GetMaterialExpressionNodePosition(E, X, Y);
			const TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("node_id"), E->GetName());
			N->SetStringField(TEXT("class"), E->GetClass()->GetName());
			N->SetNumberField(TEXT("pos_x"), X);
			N->SetNumberField(TEXT("pos_y"), Y);
			Nodes.Add(MakeShared<FJsonValueObject>(N));

			// Edges — asset-level input walk (no active editor needed).
			const int32 NumIn = E->CountInputs();
			for (int32 i = 0; i < NumIn; ++i)
			{
				const FExpressionInput* In = E->GetInput(i);
				if (!In || !In->Expression) { continue; }
				const TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
				Edge->SetStringField(TEXT("from"), In->Expression->GetName());
				Edge->SetStringField(TEXT("to"), E->GetName());
				Edge->SetStringField(TEXT("to_input"), E->GetInputName(i).ToString());
				Edges.Add(MakeShared<FJsonValueObject>(Edge));
			}
		}

		// Material output properties → connected node (asset-level).
		const TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
		for (const FMatPropEntry& PE : GMatProps)
		{
			if (const FExpressionInput* In = Mat->GetExpressionInputForProperty(PE.Prop))
			{
				if (In->Expression) { Props->SetStringField(PE.Name, In->Expression->GetName()); }
			}
		}

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("material"), Mat->GetPathName());
		Out->SetNumberField(TEXT("expression_count"), UMaterialEditingLibrary::GetNumMaterialExpressions(Mat));
		Out->SetArrayField(TEXT("expressions"), Nodes);
		Out->SetArrayField(TEXT("edges"), Edges);
		Out->SetObjectField(TEXT("properties"), Props);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	// ── M2: material instances + assignment ──────────────────────────────────────
	FAgentMcpToolResult HandleCreateMaterialInstance(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString Name, Dest, ParentPath;
		if (!Args->TryGetStringField(TEXT("name"), Name))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'name'.")); }
		if (!Args->TryGetStringField(TEXT("destination_path"), Dest))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'destination_path'.")); }
		if (!Args->TryGetStringField(TEXT("parent"), ParentPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'parent' (a Material or Material Instance).")); }
		if (!Dest.StartsWith(TEXT("/Game")))
			{ return FAgentMcpToolResult::Error(TEXT("destination_path must be under /Game.")); }

		FString Err;
		UMaterialInterface* Parent = ResolveMaterialInterface(ParentPath, Err);
		if (!Parent) { return FAgentMcpToolResult::Error(Err); }

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Parent;
		UObject* NewAsset = AssetTools.CreateAsset(Name, Dest, UMaterialInstanceConstant::StaticClass(), Factory);
		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(NewAsset);
		if (!MIC)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("CreateAsset failed for '%s' in '%s'."), *Name, *Dest)); }

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("object_path"), MIC->GetPathName());
		Out->SetStringField(TEXT("package_path"), Dest / Name);
		Out->SetStringField(TEXT("parent"), Parent->GetPathName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MicPath, ParamName, Type, Value;
		if (!Args->TryGetStringField(TEXT("material_instance"), MicPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material_instance'.")); }
		if (!Args->TryGetStringField(TEXT("parameter"), ParamName))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'parameter'.")); }
		if (!Args->TryGetStringField(TEXT("type"), Type))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'type' (scalar/vector/texture/switch).")); }
		if (!Args->TryGetStringField(TEXT("value"), Value))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'value'.")); }

		FString Err;
		UMaterialInstanceConstant* MIC = ResolveMaterialInstance(MicPath, Err);
		if (!MIC) { return FAgentMcpToolResult::Error(Err); }
		const FName Param(*ParamName);

		if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, Param, FCString::Atof(*Value));
		}
		else if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		{
			FLinearColor Color;
			if (!Color.InitFromString(Value))
				{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse vector '%s'. Use (R=..,G=..,B=..,A=..)."), *Value)); }
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, Param, Color);
		}
		else if (Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
		{
			UTexture* Tex = LoadObject<UTexture>(nullptr, *Value);
			if (!Tex)
				{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Texture not found: '%s'."), *Value)); }
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, Param, Tex);
		}
		else if (Type.Equals(TEXT("switch"), ESearchCase::IgnoreCase))
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MIC, Param, Value.ToBool());
		}
		else
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Unknown type '%s'. Use scalar/vector/texture/switch."), *Type));
		}

		UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
		MIC->MarkPackageDirty();
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleDescribeMaterialInstance(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString MicPath;
		if (!Args->TryGetStringField(TEXT("material_instance"), MicPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material_instance'.")); }
		FString Err;
		UMaterialInstanceConstant* MIC = ResolveMaterialInstance(MicPath, Err);
		if (!MIC) { return FAgentMcpToolResult::Error(Err); }

		const TSharedRef<FJsonObject> Scalars = MakeShared<FJsonObject>();
		TArray<FName> ScalarNames;
		UMaterialEditingLibrary::GetScalarParameterNames(MIC, ScalarNames);
		for (const FName& N : ScalarNames)
		{
			Scalars->SetNumberField(N.ToString(), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MIC, N));
		}

		const TSharedRef<FJsonObject> Vectors = MakeShared<FJsonObject>();
		TArray<FName> VectorNames;
		UMaterialEditingLibrary::GetVectorParameterNames(MIC, VectorNames);
		for (const FName& N : VectorNames)
		{
			Vectors->SetStringField(N.ToString(), UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(MIC, N).ToString());
		}

		const TSharedRef<FJsonObject> Textures = MakeShared<FJsonObject>();
		TArray<FName> TextureNames;
		UMaterialEditingLibrary::GetTextureParameterNames(MIC, TextureNames);
		for (const FName& N : TextureNames)
		{
			UTexture* Tex = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MIC, N);
			Textures->SetStringField(N.ToString(), Tex ? Tex->GetPathName() : FString());
		}

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("material_instance"), MIC->GetPathName());
		Out->SetStringField(TEXT("parent"), MIC->Parent ? MIC->Parent->GetPathName() : FString());
		Out->SetObjectField(TEXT("scalars"), Scalars);
		Out->SetObjectField(TEXT("vectors"), Vectors);
		Out->SetObjectField(TEXT("textures"), Textures);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleAssignMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString ActorPath, MatPath, CompName;
		if (!Args->TryGetStringField(TEXT("actor_path"), ActorPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'actor_path' (from spawn_actor/query_actors).")); }
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material'.")); }
		Args->TryGetStringField(TEXT("component"), CompName);

		AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
		if (!Actor || !IsValid(Actor))
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Actor not found: '%s'. Call query_actors for live paths."), *ActorPath)); }
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!EditorWorld || Actor->GetWorld() != EditorWorld)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Actor '%s' is not in the editor world (PIE/preview actor?)."), *ActorPath)); }

		FString Err;
		UMaterialInterface* Mat = ResolveMaterialInterface(MatPath, Err);
		if (!Mat) { return FAgentMcpToolResult::Error(Err); }

		TArray<UMeshComponent*> Meshes;
		Actor->GetComponents<UMeshComponent>(Meshes);
		UMeshComponent* Mesh = nullptr;
		if (!CompName.IsEmpty())
		{
			for (UMeshComponent* M : Meshes)
			{
				if (M && M->GetName() == CompName) { Mesh = M; break; }
			}
			if (!Mesh)
				{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("No mesh component named '%s' on the actor."), *CompName)); }
		}
		else
		{
			Mesh = Meshes.Num() > 0 ? Meshes[0] : nullptr;
			if (!Mesh)
				{ return FAgentMcpToolResult::Error(TEXT("Actor has no UMeshComponent to assign a material to.")); }
		}

		double SlotD = 0.0;
		Args->TryGetNumberField(TEXT("slot_index"), SlotD);
		const int32 Slot = static_cast<int32>(SlotD);

		Mesh->SetMaterial(Slot, Mat);
		Mesh->MarkRenderStateDirty();
		Actor->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("component"), Mesh->GetName());
		Out->SetNumberField(TEXT("slot_index"), Slot);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

}

namespace AgentMcp::Tools
{
	void RegisterMaterialTools()
	{
		using ToolUtils::RegisterOne;
		using ToolUtils::TypedProp;

		RegisterOne(TEXT("create_material"),
			TEXT("Creates a new UMaterial asset under /Game and returns {object_path, package_path}. Add nodes with add_material_expression, wire them with connect_material_*, then recompile_material. Save with save_asset."),
			{ { TEXT("name"), TypedProp(TEXT("string"), TEXT("Asset name, e.g. M_Glow.")) },
			  { TEXT("destination_path"), TypedProp(TEXT("string"), TEXT("/Game folder, e.g. /Game/Gen.")) } },
			{ TEXT("name"), TEXT("destination_path") }, &HandleCreateMaterial);

		RegisterOne(TEXT("add_material_expression"),
			TEXT("Adds a material expression node. expression_class = short name (Constant3Vector, ScalarParameter, TextureSample, Multiply, Add, TextureCoordinate) or full /Script path. Returns {node_id, object_path, class}; node_id feeds the connect_/set_ tools."),
			{ { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material.")) },
			  { TEXT("expression_class"), TypedProp(TEXT("string"), TEXT("UMaterialExpression subclass, short or full path.")) },
			  { TEXT("pos_x"), TypedProp(TEXT("number"), TEXT("Graph X (optional).")) },
			  { TEXT("pos_y"), TypedProp(TEXT("number"), TEXT("Graph Y (optional).")) } },
			{ TEXT("material"), TEXT("expression_class") }, &HandleAddMaterialExpression);

		RegisterOne(TEXT("set_material_expression_property"),
			TEXT("Sets a property on an expression node via ImportText, e.g. Constant=(R=0.1,G=0.6,B=1.0), ParameterName=Glow, DefaultValue=4.0, Texture=/Game/Tex/T_Foo.T_Foo. Returns {ok}."),
			{ { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material.")) },
			  { TEXT("node_id"), TypedProp(TEXT("string"), TEXT("Expression node id from add_material_expression / describe_material.")) },
			  { TEXT("property"), TypedProp(TEXT("string"), TEXT("Property name on the expression, e.g. Constant, ParameterName, DefaultValue, Texture.")) },
			  { TEXT("value"), TypedProp(TEXT("string"), TEXT("ImportText value string. Structs as (R=..,G=..), object refs as /Game path.")) } },
			{ TEXT("material"), TEXT("node_id"), TEXT("property"), TEXT("value") }, &HandleSetMaterialExpressionProperty);

		RegisterOne(TEXT("connect_material_expression"),
			TEXT("Connects one expression's output to another's input. Empty from_output/to_input use the first output/input. Returns {connected}."),
			{ { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material.")) },
			  { TEXT("from_node"), TypedProp(TEXT("string"), TEXT("Source expression node id.")) },
			  { TEXT("to_node"), TypedProp(TEXT("string"), TEXT("Destination expression node id.")) },
			  { TEXT("from_output"), TypedProp(TEXT("string"), TEXT("Source output name (optional; default first).")) },
			  { TEXT("to_input"), TypedProp(TEXT("string"), TEXT("Destination input name (optional; default first).")) } },
			{ TEXT("material"), TEXT("from_node"), TEXT("to_node") }, &HandleConnectMaterialExpression);

		RegisterOne(TEXT("connect_material_property"),
			TEXT("Connects an expression output to a material output property (BaseColor/Metallic/Roughness/Normal/EmissiveColor/Opacity/OpacityMask/WorldPositionOffset/…). Returns {connected}."),
			{ { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material.")) },
			  { TEXT("from_node"), TypedProp(TEXT("string"), TEXT("Source expression node id.")) },
			  { TEXT("property"), TypedProp(TEXT("string"), TEXT("Material output property name.")) },
			  { TEXT("from_output"), TypedProp(TEXT("string"), TEXT("Source output name (optional; default first).")) } },
			{ TEXT("material"), TEXT("from_node"), TEXT("property") }, &HandleConnectMaterialProperty);

		RegisterOne(TEXT("recompile_material"),
			TEXT("Lays out and recompiles a material after graph edits (required for changes to take effect). Returns {recompiled, pixel_shader_instructions, samplers}."),
			{ { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material.")) } },
			{ TEXT("material") }, &HandleRecompileMaterial);

		RegisterOne(TEXT("describe_material"),
			TEXT("Reads a material graph directly from the asset (works headless): {expression_count, expressions:[{node_id,class,pos_x,pos_y}], edges:[{from,to,to_input}], properties:{BaseColor:node_id,…}}. Use for verification and to recover node ids."),
			{ { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material.")) } },
			{ TEXT("material") }, &HandleDescribeMaterial);

		RegisterOne(TEXT("create_material_instance"),
			TEXT("Creates a UMaterialInstanceConstant under /Game parented to a Material or Material Instance. Returns {object_path, package_path, parent}. Set overrides with set_material_instance_parameter."),
			{ { TEXT("name"), TypedProp(TEXT("string"), TEXT("Asset name, e.g. MI_Glow_Red.")) },
			  { TEXT("destination_path"), TypedProp(TEXT("string"), TEXT("/Game folder.")) },
			  { TEXT("parent"), TypedProp(TEXT("string"), TEXT("/Game path of the parent Material or Material Instance.")) } },
			{ TEXT("name"), TEXT("destination_path"), TEXT("parent") }, &HandleCreateMaterialInstance);

		RegisterOne(TEXT("set_material_instance_parameter"),
			TEXT("Overrides a parameter on a Material Instance. type = scalar (value=float), vector (value=(R=..,G=..,B=..,A=..)), texture (value=/Game texture path), or switch (value=true/false). Returns {ok}."),
			{ { TEXT("material_instance"), TypedProp(TEXT("string"), TEXT("/Game path of the Material Instance.")) },
			  { TEXT("parameter"), TypedProp(TEXT("string"), TEXT("Parameter name on the parent material.")) },
			  { TEXT("type"), TypedProp(TEXT("string"), TEXT("scalar | vector | texture | switch.")) },
			  { TEXT("value"), TypedProp(TEXT("string"), TEXT("Value string; format depends on type.")) } },
			{ TEXT("material_instance"), TEXT("parameter"), TEXT("type"), TEXT("value") }, &HandleSetMaterialInstanceParameter);

		RegisterOne(TEXT("describe_material_instance"),
			TEXT("Reads a Material Instance's parent and overridable parameters: {parent, scalars:{name:value}, vectors:{name:'(R=..)'}, textures:{name:path}}."),
			{ { TEXT("material_instance"), TypedProp(TEXT("string"), TEXT("/Game path of the Material Instance.")) } },
			{ TEXT("material_instance") }, &HandleDescribeMaterialInstance);

		RegisterOne(TEXT("assign_material"),
			TEXT("Assigns a Material or Material Instance to a placed editor-world actor's mesh component slot. component defaults to the actor's first UMeshComponent; slot_index defaults to 0. Returns {ok, component, slot_index}."),
			{ { TEXT("actor_path"), TypedProp(TEXT("string"), TEXT("Actor object path from spawn_actor/query_actors.")) },
			  { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the Material or Material Instance.")) },
			  { TEXT("component"), TypedProp(TEXT("string"), TEXT("Mesh component name (optional; default first mesh component).")) },
			  { TEXT("slot_index"), TypedProp(TEXT("number"), TEXT("Material slot index (optional, default 0).")) } },
			{ TEXT("actor_path"), TEXT("material") }, &HandleAssignMaterial);
	}
}
