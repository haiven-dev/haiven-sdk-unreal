using UnrealBuildTool;

public class HaivenEditor : ModuleRules
{
	public HaivenEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Haiven",
			"Slate",
			"SlateCore",
			"UnrealEd",
		});
	}
}
