#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Blueprint/HaivenBlueprintRestConvert.h"
#include "Blueprint/HaivenBlueprintRestOperations.h"
#include "Contracts/HaivenRestJson.h"

// The typed REST layer (codegen/emit_rest_types.py): the components' Blueprint
// mirrors and converters, and the typed operation nodes' reflected pins. The
// codec is sdks/cpp/test/rest_json_test.cpp's; this proves the mirrors carry
// what it reads (a map of components, a list, a 64-bit integer) and that the
// nodes build under UHT with their typed body and reply.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenBlueprintRestTypes,
	"Haiven.Blueprint.RestTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenBlueprintRestTypes::RunTest(const FString& Parameters)
{
	Haiven::RestTypes::StoreApp App;
	std::string Error;
	TestTrue(TEXT("a store page parses"), Haiven::ContractsJson::FromJson(
		R"({"id":"a-1","steamId":9007199254740993,"tags":[{"id":"t","name":"Puzzle"}],"systemRequirements":{"windows":{"minimum":{}}}})",
		App, Error));

	const FHaivenRestStoreApp Mirror = Haiven::Blueprint::FromCpp(App);
	TestTrue(TEXT("an optional member keeps its presence"), Mirror.bHasId && Mirror.Id == TEXT("a-1") && !Mirror.bHasName);
	TestTrue(TEXT("a 64-bit integer"), Mirror.bHasSteamId && Mirror.SteamId == 9007199254740993LL);
	TestTrue(TEXT("a list of components"), Mirror.bHasTags && Mirror.Tags.Num() == 1 && Mirror.Tags[0].Name == TEXT("Puzzle"));
	TestTrue(TEXT("a map of components"), Mirror.bHasSystemRequirements && Mirror.SystemRequirements.Contains(TEXT("windows"))
		&& Mirror.SystemRequirements[TEXT("windows")].bHasMinimum);

	const Haiven::RestTypes::StoreApp Back = Haiven::Blueprint::ToCpp(Mirror);
	TestTrue(TEXT("which converts back"), Back.SystemRequirements.has_value() && Back.SystemRequirements->count("windows") == 1
		&& Back.SteamId == std::optional<std::int64_t>(9007199254740993LL));

	const UFunction* Login = UHaivenRestPostAuthLoginAsyncAction::StaticClass()->FindFunctionByName(TEXT("RestPostAuthLogin"));
	TestTrue(TEXT("an operation node is Blueprint-callable"), Login != nullptr && Login->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	TestNotNull(TEXT("with a typed body pin"), Login ? Login->FindPropertyByName(TEXT("Body")) : nullptr);
	TestNotNull(TEXT("and a typed reply"), UHaivenRestGetAppsAppIdAsyncAction::StaticClass()->FindPropertyByName(TEXT("Completed")));
	TestNotNull(TEXT("and a failure pin"), UHaivenRestGetAppsAppIdAsyncAction::StaticClass()->FindPropertyByName(TEXT("Failed")));
	return true;
}
#endif
