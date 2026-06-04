#include "Tools/PropertyBridge.h"

#include "Misc/OutputDevice.h"
#include "UObject/UnrealType.h"

namespace
{
	FProperty* ResolveProperty(UObject* Container, const FString& PropertyName, FString& OutError)
	{
		FProperty* Property = Container->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on '%s'."), *PropertyName, *Container->GetClass()->GetName());
		}
		return Property;
	}

	/** Captures ImportText error output instead of letting it spam GWarn. */
	class FImportErrorCapture final : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			if (!Captured.IsEmpty()) { Captured += TEXT("; "); }
			Captured += V;
		}
	};
}

bool AgentMcp::PropertyBridge::GetPropertyAsString(UObject* Container, const FString& PropertyName, FString& OutValue, FString& OutType, FString& OutError)
{
	FProperty* Property = ResolveProperty(Container, PropertyName, OutError);
	if (!Property)
	{
		return false;
	}
	OutType = Property->GetCPPType();
	OutValue.Reset();
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
	// ExportText_Direct(FString& ValueStr, const void* Data, const void* Delta, UObject* Parent,
	//                   int32 PortFlags, UObject* ExportRootScope = nullptr) const
	// Passing ValuePtr for both Data and Delta: Delta is the "default" comparand used by some
	// serializers to skip unchanged values — passing the live value as its own delta means
	// "always export" (no suppression by equality with a CDO default).
	Property->ExportText_Direct(OutValue, ValuePtr, ValuePtr, Container, PPF_None);
	return true;
}

bool AgentMcp::PropertyBridge::SetPropertyFromString(UObject* Container, const FString& PropertyName, const FString& Value, FString& OutError)
{
	FProperty* Property = ResolveProperty(Container, PropertyName, OutError);
	if (!Property)
	{
		return false;
	}
	if (!Property->HasAnyPropertyFlags(CPF_Edit))
	{
		OutError = FString::Printf(TEXT("Property '%s' is not editable (needs EditAnywhere/EditDefaultsOnly); use a BlueprintCallable setter instead."), *PropertyName);
		return false;
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
	FImportErrorCapture Errors;
	const TCHAR* Result = Property->ImportText_Direct(*Value, ValuePtr, Container, PPF_None, &Errors);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to parse '%s' for property '%s' (%s)%s%s"),
			*Value, *PropertyName, *Property->GetCPPType(),
			Errors.Captured.IsEmpty() ? TEXT("") : TEXT(": "), *Errors.Captured);
		return false;
	}
	return true;
}
