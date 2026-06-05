#pragma once

#include "Tools/NodeGraphUtils.h"
#include "WidgetBlueprint.h"

/**
 * Inline helpers shared between WidgetTools.cpp and WidgetEditTools.cpp.
 * Lives in namespace AgentMcp::WidgetShared to avoid ODR collisions.
 */
namespace AgentMcp::WidgetShared
{
	/**
	 * Loads a blueprint and casts it to UWidgetBlueprint.
	 * Returns nullptr + error when not a widget BP.
	 */
	inline UWidgetBlueprint* ResolveWidgetBlueprint(const FString& Path, FString& OutError)
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

} // namespace AgentMcp::WidgetShared
