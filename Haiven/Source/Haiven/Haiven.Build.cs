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

		// The protocol, session and chat cores and the generated surface are the
		// C++ SDK (sdks/cpp): plain C++17 with no engine types, compiled and tested
		// under an ordinary compiler. Unreal builds them too, through HaivenCpp,
		// using the C++20 mode required by the primary UE 5.8 target.
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// Public, so a game module that includes this plugin's headers also
			// gets the C++ SDK headers they include.
			"HaivenCpp",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"HTTP",
			// The engine's embedded HTTP server receives the loopback redirect of
			// BeginLoopbackGameLogin; Sockets supplies the peer address it checks.
			"HTTPServer",
			"Sockets",
			// No "Json": frames, payloads and the handoff response are parsed by
			// the C++ SDK's reader (HaivenJson.h), the one its tests prove.
			"WebSockets",
		});


		// The handoff's state and verifier entropy come from the engine-pinned
		// OpenSSL RAND_bytes on every supported platform; neither FMath::Rand nor
		// GUIDs provide 256 bits of cryptographic entropy. (The S256 hash itself
		// is the C++ SDK's, in HaivenHandoffCore.h.)
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
