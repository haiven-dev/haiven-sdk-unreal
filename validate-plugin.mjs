#!/usr/bin/env node
// Check the plugin descriptor and build rules against what Unreal enforces at
// load, since Unreal is not here to enforce them.
//
// The same split as the UPM package validator: the layout half of an engine
// integration is a set of rules with one right answer each, and none of them
// needs the engine to check. What they have in common is that the engine's own
// failure for each is unhelpful — a module whose name does not match its
// `Build.cs` class fails to build with an error about the class, not about the
// name — so catching them here costs nothing and saves the slowest feedback loop
// in this stack.
//
// This is not a substitute for loading the plugin in an editor. See README.md
// for what remains unverified.
//
//   node validate-plugin.mjs

import { existsSync, readFileSync, readdirSync } from "node:fs"
import { dirname, join } from "node:path"
import { fileURLToPath } from "node:url"

// URL.pathname is "/C:/..." on Windows. path.join then builds "\C:\...\Haiven"
// and readdirSync throws ENOENT before any descriptor check.
const HERE = dirname(fileURLToPath(import.meta.url))
const PLUGIN = join(HERE, "Haiven")

const problems = []
const check = (condition, message) => {
	if (!condition) problems.push(message)
}

const descriptors = readdirSync(PLUGIN).filter((f) => f.endsWith(".uplugin"))
check(descriptors.length === 1, `expected exactly one .uplugin, found ${descriptors.length}`)

const descriptor = JSON.parse(readFileSync(join(PLUGIN, descriptors[0]), "utf8"))

// Unreal keys a plugin by its descriptor's file name, not by anything inside
// it. A folder that disagrees loads under a name nothing references.
check(descriptors[0] === "Haiven.uplugin", `descriptor name and plugin folder disagree: ${descriptors[0]}`)

// FileVersion 3 is the current descriptor schema. An older value is accepted and
// silently changes how some fields are read, which is worse than a rejection.
check(descriptor.FileVersion === 3, `FileVersion must be 3, found ${descriptor.FileVersion}`)

for (const field of ["Version", "VersionName", "FriendlyName", "Description", "Category"]) {
	check(descriptor[field] !== undefined, `.uplugin is missing ${field}`)
}

// EngineVersion is the single-version gate, and it is measured rather than
// assumed: with "5.8.0" in this descriptor, UE 5.6.1 refuses the plugin with
// "The 'Haiven' plugin was designed for build 5.8.0" and Haiven.Plugin.Loads
// fails. It is not a minimum — the engine compares against its own COMPATIBLE
// version, so any fixed value excludes every other engine, including the 5.8.2
// that reports 5.8.0. Omitting it is what lets one source plugin load in all of
// them; the per-engine pins live in ci/engines.json instead. A precompiled
// distribution is the exception and stamps this field per version at packaging
// time, because those binaries genuinely are version-specific.
check(descriptor.EngineVersion === undefined,
	`.uplugin declares EngineVersion ${descriptor.EngineVersion}; that pins the plugin to one engine (see ci/engines.json)`)

check(Array.isArray(descriptor.Modules) && descriptor.Modules.length > 0, ".uplugin declares no modules")

for (const module of descriptor.Modules ?? []) {
	check(typeof module.Name === "string" && module.Name !== "", "a module has no Name")
	check(typeof module.Type === "string", `${module.Name}: no Type`)

	// A Runtime module without a LoadingPhase defaults to Default, which is
	// after most engine subsystems are up. For a module that registers one, that
	// is a startup-order bug that reproduces intermittently.
	if (module.Type === "Runtime") {
		check(typeof module.LoadingPhase === "string", `${module.Name}: a Runtime module should state its LoadingPhase`)
	}

	const source = join(PLUGIN, "Source", module.Name)
	check(existsSync(source), `${module.Name}: no Source/${module.Name} directory`)

	const rules = join(source, `${module.Name}.Build.cs`)
	check(existsSync(rules), `${module.Name}: no ${module.Name}.Build.cs`)

	if (!existsSync(rules)) continue

	const build = readFileSync(rules, "utf8")

	// UnrealBuildTool finds the rules class by name. A mismatch fails the build
	// with an error about a missing class rather than about the module.
	check(
		new RegExp(`class\\s+${module.Name}\\s*:\\s*ModuleRules`).test(build),
		`${module.Name}.Build.cs does not declare "class ${module.Name} : ModuleRules"`,
	)

	// The engine dependency is the WebSockets MODULE. WebSocketNetworking is a
	// plugin for replication transport and is a different thing; reaching for it
	// by name is the mistake this checks against.
	check(!/PublicDependencyModuleNames[\s\S]{0,400}WebSocketNetworking/.test(build), `${module.Name}: depends on the WebSocketNetworking plugin; the engine module is "WebSockets"`)
}

// The multi-version trap that fires FIRST is not C++ but the C# rules
// assembly: BuildSettingsVersion.V7 and EngineIncludeOrderVersion.Unreal5_8 are
// enum members that older UnrealBuildTool does not define, so a 5.6 build stops
// at "does not contain a definition for 'V7'" before a single translation unit
// is compiled. `Latest` is defined in every supported version and means "this
// engine's newest", which is the right answer for a plugin that is always built
// from source. Measured: with V7/Unreal5_8 pinned, UE 5.6.1 fails at RulesError.
const rules = join(HERE, "host/Source")
for (const file of readdirSync(rules).filter((f) => f.endsWith(".Target.cs"))) {
	const source = readFileSync(join(rules, file), "utf8")
	const pinned = source.match(/BuildSettingsVersion\.V\d+|EngineIncludeOrderVersion\.Unreal\d+_\d+/g)
	check(pinned === null, `${file} pins ${pinned?.join(", ")}; use Latest so older UnrealBuildTool can compile these rules`)
}

// These are the engine-only half of the binding. The ordinary compiler cannot
// parse UHT macros or engine module headers, but the layout gate can still stop
// a refactor from quietly dropping one of the promised integration surfaces.
const runtimeSource = join(PLUGIN, "Source", "Haiven")
const networkingPath = join(runtimeSource, "Private", "HaivenNetworking.cpp")
const actionsPath = join(runtimeSource, "Public", "HaivenAsyncActions.h")
const sessionPath = join(runtimeSource, "Public", "HaivenSessionCore.h")
for (const path of [networkingPath, actionsPath, sessionPath]) {
	check(existsSync(path), `missing runtime SDK surface: ${path.slice(PLUGIN.length + 1)}`)
}
if (existsSync(networkingPath)) {
	const source = readFileSync(networkingPath, "utf8")
	check(source.includes("FHttpModule::Get()"), "runtime client does not use FHttpModule")
	check(source.includes("FWebSocketsModule::Get().CreateWebSocket"), "runtime client does not create an IWebSocket transport")
	check(source.includes("DispatchToGameThread"), "runtime callbacks have no game-thread dispatch boundary")
}
if (existsSync(actionsPath)) {
	const source = readFileSync(actionsPath, "utf8")
	check(source.match(/UBlueprintAsyncActionBase/g)?.length === 3, "expected HTTP, connect, and realtime Blueprint async actions")
}

check(existsSync(join(HERE, "README.md")), "spike has no README.md")

if (problems.length > 0) {
	for (const problem of problems) console.error(`invalid: ${problem}`)
	process.exit(1)
}

console.error("Haiven.uplugin: descriptor and build rules valid")
console.error("NOT a substitute for loading the plugin in an editor — see README.md")
