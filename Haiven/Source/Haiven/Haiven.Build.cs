// The module's build rules.
//
// `WebSockets` and `HTTP` are engine modules rather than plugins, which is why
// the `.uplugin` does not depend on the WebSocketNetworking plugin — that one is
// for replication transport and is a different thing entirely. Naming it as a
// disabled dependency there is deliberate: it is the plugin somebody reaches for
// by name, and being explicit that it is NOT the one costs a line.

using UnrealBuildTool;

public class Haiven : ModuleRules
{
	public Haiven(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The protocol core is plain C++17 with no engine types, so it compiles
		// under an ordinary compiler and is tested that way. Unreal builds it
		// too, using the C++20 mode required by the primary UE 5.8 target.
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"HTTP",
			"Json",
			"WebSockets",
		});


		// PKCE and state use the engine-pinned OpenSSL implementation on every
		// supported platform; neither FMath::Rand nor GUIDs provide 256 bits of
		// cryptographic entropy.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
