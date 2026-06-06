using UnrealBuildTool;

public class UnrealAgentMCP : ModuleRules
{
	public UnrealAgentMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTPServer",
			"AssetRegistry",
			"BlueprintGraph",
			"UnrealEd",
			"BlueprintEditorLibrary",
			"SubobjectDataInterface",
			"EnhancedInput",
			"InputCore",
			"UMG",
			"UMGEditor",
			"ModelViewViewModelEditor",
			"ModelViewViewModelBlueprint",
			"ModelViewViewModel",
			"FieldNotification",
			"MovieScene",
			"GameplayAbilities",
			"GameplayTags",
			"AIModule",
		});
	}
}
