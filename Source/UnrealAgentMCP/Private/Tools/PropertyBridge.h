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
	 * bRejectTemplateDisabled: pass true when Container is a CDO/archetype — rejects EditInstanceOnly
	 * properties (CPF_DisableEditOnTemplate), matching what the Details panel allows on templates.
	 * Instance callers (actors, components) leave it false: EditInstanceOnly is legal there.
	 *
	 * KNOWN LIMITATIONS (whole-value semantics only):
	 * - Containers (TArray/TSet/TMap) are replaced wholesale via ImportText syntax "(a,b,c)";
	 *   single-element insert/remove and TMap key addressing are not supported.
	 * - Delegate / multicast-delegate properties cannot be meaningfully set via text import.
	 */
	bool SetPropertyFromString(UObject* Container, const FString& PropertyName, const FString& Value, FString& OutError, bool bRejectTemplateDisabled = false);
}
