#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenAsyncActions.h"
#include "HaivenSubsystem.h"
#include "Kismet/BlueprintAsyncActionBase.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenClientSurface,
	"Haiven.Client.ReflectedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenClientSurface::RunTest(const FString& Parameters)
{
	UClass* Subsystem = UHaivenSubsystem::StaticClass();
	TestTrue(TEXT("entry point is a game-instance subsystem"), Subsystem->IsChildOf(UGameInstanceSubsystem::StaticClass()));
	TestNotNull(TEXT("Blueprint configure entry point"), Subsystem->FindFunctionByName(TEXT("Configure")));
	TestNotNull(TEXT("Blueprint disconnect entry point"), Subsystem->FindFunctionByName(TEXT("DisconnectRealtime")));

	TestTrue(TEXT("HTTP request is an async Blueprint node"),
		UHaivenHttpRequestAsyncAction::StaticClass()->IsChildOf(UBlueprintAsyncActionBase::StaticClass()));
	TestTrue(TEXT("authenticated connect is an async Blueprint node"),
		UHaivenConnectRealtimeAsyncAction::StaticClass()->IsChildOf(UBlueprintAsyncActionBase::StaticClass()));
	TestTrue(TEXT("correlated request is an async Blueprint node"),
		UHaivenRealtimeRequestAsyncAction::StaticClass()->IsChildOf(UBlueprintAsyncActionBase::StaticClass()));
	TestNotNull(TEXT("HTTP factory is reflected"),
		UHaivenHttpRequestAsyncAction::StaticClass()->FindFunctionByName(TEXT("HttpRequest")));
	TestNotNull(TEXT("connect factory is reflected"),
		UHaivenConnectRealtimeAsyncAction::StaticClass()->FindFunctionByName(TEXT("ConnectRealtime")));
	TestNotNull(TEXT("realtime factory is reflected"),
		UHaivenRealtimeRequestAsyncAction::StaticClass()->FindFunctionByName(TEXT("RealtimeRequest")));
	return true;
}
#endif
