#pragma once

#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "MVVMEditorSubsystem.h"
#include "Tools/NodeGraphUtils.h"
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMConversionFunctionValue.h"
#include "Types/MVVMFieldVariant.h"
#include "WidgetBlueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Inline helpers shared between MvvmTools.cpp (add_viewmodel) and
 * MvvmBindingTools.cpp (add_view_binding, list_view_bindings, remove_view_binding).
 *
 * All helpers live in namespace AgentMcp::MvvmShared to avoid ODR collisions
 * when both translation units include this header.
 */
namespace AgentMcp::MvvmShared
{
	/** Resolve a WidgetBlueprint by path. */
	inline UWidgetBlueprint* ResolveWBP(const FString& Path, FString& OutError)
	{
		UBlueprint* BP = NodeGraphUtils::ResolveBlueprint(Path, OutError);
		if (!BP)
		{
			return nullptr;
		}
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
		if (!WBP)
		{
			OutError = FString::Printf(TEXT("'%s' is not a WidgetBlueprint."), *Path);
			return nullptr;
		}
		return WBP;
	}

	/** Collect compile result fields — mirrors BlueprintTools.cpp HandleCompileBlueprint output. */
	inline TSharedRef<FJsonObject> RunCompileAndCollect(UBlueprint* BP)
	{
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("severity"),
				Msg->GetSeverity() == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Entry->SetStringField(TEXT("text"), Msg->ToText().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> CompileObj = MakeShared<FJsonObject>();
		CompileObj->SetStringField(TEXT("status"), Results.NumErrors > 0 ? TEXT("error") : TEXT("ok"));
		CompileObj->SetNumberField(TEXT("num_errors"), Results.NumErrors);
		CompileObj->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
		CompileObj->SetArrayField(TEXT("messages"), Messages);
		return CompileObj;
	}

	/** Reverse of ParseBindingMode, for list_view_bindings. */
	inline const TCHAR* BindingModeToToken(EMVVMBindingMode Mode)
	{
		switch (Mode)
		{
		case EMVVMBindingMode::OneWayToDestination:  return TEXT("one_way");
		case EMVVMBindingMode::TwoWay:               return TEXT("two_way");
		case EMVVMBindingMode::OneTimeToDestination: return TEXT("one_time");
		case EMVVMBindingMode::OneWayToSource:       return TEXT("one_way_to_source");
		default:                                     return TEXT("unknown");
		}
	}

	/**
	 * First conversion function able to turn ArgType into RetType, preferring "simple"
	 * single-argument pure functions (what the View Bindings panel surfaces first).
	 */
	inline const UFunction* FindConversionFunction(UMVVMEditorSubsystem* Sub, UWidgetBlueprint* WBP,
		const FProperty* ArgType, const FProperty* RetType)
	{
		const TArray<UE::MVVM::FConversionFunctionValue> Candidates = Sub->GetConversionFunctions(WBP, ArgType, RetType);
		for (const UE::MVVM::FConversionFunctionValue& Candidate : Candidates)
		{
			if (Candidate.IsFunction() && Sub->IsSimpleConversionFunction(Candidate.GetFunction()))
			{
				return Candidate.GetFunction();
			}
		}
		for (const UE::MVVM::FConversionFunctionValue& Candidate : Candidates)
		{
			if (Candidate.IsFunction())
			{
				return Candidate.GetFunction();
			}
		}
		return nullptr;
	}

	/** Parse binding direction token. */
	inline bool ParseBindingMode(const FString& Token, EMVVMBindingMode& OutMode, FString& OutError)
	{
		const FString Lower = Token.ToLower();
		if (Lower == TEXT("one_way") || Lower == TEXT("oneway"))
		{
			OutMode = EMVVMBindingMode::OneWayToDestination;
			return true;
		}
		if (Lower == TEXT("two_way") || Lower == TEXT("twoway"))
		{
			OutMode = EMVVMBindingMode::TwoWay;
			return true;
		}
		if (Lower == TEXT("one_time") || Lower == TEXT("onetime"))
		{
			OutMode = EMVVMBindingMode::OneTimeToDestination;
			return true;
		}
		if (Lower == TEXT("one_way_to_source") || Lower == TEXT("onewaytosource"))
		{
			OutMode = EMVVMBindingMode::OneWayToSource;
			return true;
		}
		OutError = FString::Printf(
			TEXT("Unknown direction '%s'. Valid values: one_way (viewmodel->widget), two_way, one_time, one_way_to_source (widget->viewmodel)."), *Token);
		return false;
	}

	/** First input (non-return, non-out) parameter name of a function — the conversion data pin. */
	inline FName FirstInputParamName(const UFunction* Function)
	{
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				return It->GetFName();
			}
		}
		return NAME_None;
	}

} // namespace AgentMcp::MvvmShared
