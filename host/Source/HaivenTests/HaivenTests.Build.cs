using UnrealBuildTool;
public class HaivenTests : ModuleRules
{
    public HaivenTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Projects", "Json", "Haiven" });
    }
}
