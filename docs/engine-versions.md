# Supporting more than one engine version

The plugin is tested on Linux with Unreal Engine **5.8.2** (primary) and
**5.6.1**. Other engine versions and platforms are outside that run.

One source tree, built once per engine. Nothing under `Haiven/` or
`host/` names a version. `ci/engines.json` holds the matrix, and
`ci/run.py` selects a row.

Three constraints were measured on 5.8.2 and 5.6.1:

- **`EngineVersion` stays unset in source.** Unreal compares that field
  with the engine's compatible version, not as a minimum, so a fixed
  value loads in only one engine. With `5.8.0` in the descriptor, UE
  5.6.1 refuses the plugin ("The 'Haiven' plugin was designed for build
  5.8.0"). Omitting the field is what lets one descriptor load in both. A
  prebuilt zip stamps the field for its own engine at pack time, so use a
  prebuilt zip only with the engine named in its file name. The source zip
  does not stamp that field.
- **Target rules use `Latest`.** The host targets under `host/Source/` set
  `BuildSettingsVersion.Latest` and `EngineIncludeOrderVersion.Latest`.
  `BuildSettingsVersion.V7` and `EngineIncludeOrderVersion.Unreal5_8` are
  unknown to older UnrealBuildTool, so 5.6 fails while compiling the C#
  rules assembly (`CS0117`), before any translation unit is built.
  `Latest` means this engine's newest settings, which is what a
  from-source plugin wants.
- **`EngineAssociation` is per engine.** Tracked
  `host/HaivenTests.uproject` leaves it empty. `ci/run.py` writes the
  selected engine's major.minor into the staged copy.

The plain C++ core has no version guards. Runtime glue stays on APIs this
UE5 line shares: `UGameInstanceSubsystem`, `FHttpModule`, `IWebSocket`,
and `UBlueprintAsyncActionBase`.

Adding an engine means a new `ci/engines.json` row and a passing run of
that row. It does not widen the claim on its own.
