#include "Tools/MaterialTools.h"

#include "AssetToolsModule.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
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

	// ── schema sugar (keeps registrations to a few lines each) ────────────────────
	TSharedRef<FJsonObject> TypedProp(const TCHAR* Type, const FString& Desc)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	}
	TSharedPtr<FJsonObject> MakeSchema(
		const TArray<TPair<FString, TSharedRef<FJsonObject>>>& Props, const TArray<FString>& Required)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Props) { P->SetObjectField(Pair.Key, Pair.Value); }
		Schema->SetObjectField(TEXT("properties"), P);
		TArray<TSharedPtr<FJsonValue>> Req;
		for (const FString& R : Required) { Req.Add(MakeShared<FJsonValueString>(R)); }
		Schema->SetArrayField(TEXT("required"), Req);
		return Schema;
	}

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

	void RegisterOne(const TCHAR* Name, const FString& Desc,
		const TArray<TPair<FString, TSharedRef<FJsonObject>>>& Props, const TArray<FString>& Required,
		FAgentMcpToolResult (*Fn)(const TSharedPtr<FJsonObject>&))
	{
		FAgentMcpToolDef Def;
		Def.Name = Name;
		Def.Description = Desc;
		Def.InputSchema = MakeSchema(Props, Required);
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(Fn);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}

namespace AgentMcp::Tools
{
	void RegisterMaterialTools()
	{
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
	}
}
