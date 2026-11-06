// The C++ SDK, vendored: header-only, engine-free C++17.
//
// `include/` is a byte-for-byte copy of sdks/cpp/include, written by
// scripts/vendor-cpp.py and checked by `make check-cpp-vendor`. Edit or
// regenerate sdks/cpp, never this copy. It lives inside the plugin because a
// game installs the plugin by copying its folder, and a header included from
// outside that folder would not exist there.
//
// An External module builds nothing; it exists to put the headers on the
// include path of every module that depends on it. They are deliberately not
// system includes, so the engine's warning set still applies to them.

using System.IO;
using UnrealBuildTool;

public class HaivenCpp : ModuleRules
{
	public HaivenCpp(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));
	}
}
