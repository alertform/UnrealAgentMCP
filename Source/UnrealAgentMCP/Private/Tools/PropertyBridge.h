#pragma once

#include "CoreMinimal.h"

class FProperty;

namespace AgentMcp::PropertyBridge
{
	/**
	 * Resolves Property by name on Container's class, exports its value as a UE-text string.
	 * Returns false + OutError on unknown property. OutType receives the property's CPP type name.
	 */
	bool GetPropertyAsString(UObject* Container, const FString& PropertyName, FString& OutValue, FString& OutType, FString& OutError);

	/**
	 * Resolves Property by name, requires CPF_Edit (mirrors Remote Control's constraint), imports
	 * Value via ImportText_Direct. Returns false + OutError on unknown/uneditable property or parse
	 * failure (the engine's import error text is included). Caller owns Modify()/transactions.
	 */
	bool SetPropertyFromString(UObject* Container, const FString& PropertyName, const FString& Value, FString& OutError);
}
