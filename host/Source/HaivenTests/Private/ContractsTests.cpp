#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Blueprint/HaivenBlueprintContracts.h"
#include "Blueprint/HaivenBlueprintConvert.h"
#include "Contracts/HaivenContracts.h"
#include "Vocabulary/ErrorCode.h"
#include "Vocabulary/MessageType.h"
#include "Vocabulary/WsErrorCode.h"

// The generated contract and vocabulary headers under the engine's own compiler
// and warning set. sdks/cpp/test/contracts_test.cpp proves the same headers under
// plain C++17; this proves the plugin carries them and they build in a module.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenGeneratedContracts,
	"Haiven.Contracts.Generated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenGeneratedContracts::RunTest(const FString& Parameters)
{
	Haiven::WsServerReplyV0 Event;
	TestFalse(TEXT("an event's success is absent, not false"), Event.Success.has_value());

	const haiven::wire::ErrorCode Minted{"studio_quota_exceeded"};
	TestFalse(TEXT("an unknown code is representable but unrecognised"), Minted.recognised());
	TestTrue(TEXT("a known code is recognised"), haiven::wire::ErrorCode::kRateLimited.recognised());
	return true;
}

// The Blueprint projection (Blueprint/*.h, from codegen/emit_blueprint.py): the
// mirrors and nodes a Blueprint user sees, exercised through the same static
// functions the nodes call and checked through reflection. The codec under them
// is sdks/cpp/test/contracts_json_test.cpp's; this proves the mapping onto
// Blueprint types holds under the engine's compiler and UHT.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenBlueprintContracts,
	"Haiven.Contracts.Blueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenBlueprintContracts::RunTest(const FString& Parameters)
{
	FString Error;

	FHaivenWsServerReplyV0 Reply;
	TestTrue(TEXT("a reply parses"), UHaivenBlueprintContracts::ParseWsServerReplyV0(
		TEXT(R"({"type":"chat.send","requestId":"r-1","success":true,"payload":{"messageId":"m-9"},"later":1})"), Reply, Error));
	TestEqual(TEXT("type"), Reply.Type, FString(TEXT("chat.send")));
	TestTrue(TEXT("success is present and true"), Reply.bHasSuccess && Reply.Success);
	TestTrue(TEXT("payload is present"), Reply.bHasPayload);
	TestFalse(TEXT("error is absent"), Reply.bHasError);

	FHaivenWsServerReplyV0 Echo;
	TestTrue(TEXT("a reply written as JSON parses back"), UHaivenBlueprintContracts::ParseWsServerReplyV0(
		UHaivenBlueprintContracts::WsServerReplyV0ToJson(Reply), Echo, Error));
	TestEqual(TEXT("round-tripped request id"), Echo.RequestId, Reply.RequestId);
	TestTrue(TEXT("round-tripped presence"), Echo.bHasSuccess && Echo.bHasPayload && !Echo.bHasError);

	FHaivenWsServerReplyV0 Event;
	TestTrue(TEXT("an event parses"), UHaivenBlueprintContracts::ParseWsServerReplyV0(
		TEXT(R"({"type":"chat.message","requestId":""})"), Event, Error));
	TestFalse(TEXT("an event's success is absent, not false"), Event.bHasSuccess);
	TestFalse(TEXT("which survives conversion to C++"), Haiven::Blueprint::ToCpp(Event).Success.has_value());

	FHaivenEntitlementsUpdatedV0 Entitlements;
	Entitlements.ProjectionVersion = static_cast<int64>(MAX_int32);
	TestEqual(TEXT("the largest C++ integer writes without loss"),
		UHaivenBlueprintContracts::EntitlementsUpdatedV0ToJson(Entitlements),
		FString(TEXT(R"({"projectionVersion":2147483647})")));
	Entitlements.ProjectionVersion = static_cast<int64>(MAX_int32) + 1;
	TestTrue(TEXT("an out-of-range Blueprint integer is rejected before narrowing"),
		UHaivenBlueprintContracts::EntitlementsUpdatedV0ToJson(Entitlements).IsEmpty());

	TestFalse(TEXT("a missing required field fails"), UHaivenBlueprintContracts::ParseWsServerReplyV0(
		TEXT(R"({"type":"chat.send"})"), Reply, Error));
	TestEqual(TEXT("and names it"), Error, FString(TEXT("requestId: missing")));

	FHaivenWsDroppedFramesV0 Dropped;
	TestTrue(TEXT("nested lists parse"), UHaivenBlueprintContracts::ParseWsDroppedFramesV0(
		TEXT(R"({"count":3,"types":[{"type":"chat.send","count":2}],"requestIds":["a","b"],"truncated":false})"), Dropped, Error));
	TestTrue(TEXT("list of records"), Dropped.Types.Num() == 1 && Dropped.Types[0].Count == 2);
	TestEqual(TEXT("list of strings"), Dropped.RequestIds.Num(), 2);

	TArray<FHaivenNotificationListV0Item> Notifications;
	TestTrue(TEXT("a list contract parses"), UHaivenBlueprintContracts::ParseNotificationListV0(
		TEXT(R"([{"id":"n-1","userId":"u-1","kind":"system","variety":"info","title":"Hi","body":"B","data":"{}","isRead":false,"createdAt":"2026-01-01T00:00:00Z","updatedAt":"2026-01-01T00:00:00Z"}])"),
		Notifications, Error));
	TestTrue(TEXT("one notification"), Notifications.Num() == 1 && Notifications[0].Id == TEXT("n-1"));

	TestTrue(TEXT("a known type maps to its enum"), UHaivenBlueprintContracts::ToMessageType(TEXT("chat.send")) == EHaivenMessageType::ChatSend);
	TestTrue(TEXT("an unknown type is Unrecognized"), UHaivenBlueprintContracts::ToMessageType(TEXT("chat.future")) == EHaivenMessageType::Unrecognized);
	TestEqual(TEXT("an enum maps back to its wire value"), UHaivenBlueprintContracts::MessageTypeToString(EHaivenMessageType::ChatSend), FString(TEXT("chat.send")));

	const UScriptStruct* Struct = FHaivenWsServerReplyV0::StaticStruct();
	TestNotNull(TEXT("presence is a reflected member"), Struct->FindPropertyByName(TEXT("bHasSuccess")));
	const UFunction* Parse = UHaivenBlueprintContracts::StaticClass()->FindFunctionByName(TEXT("ParseWsServerReplyV0"));
	TestTrue(TEXT("the parse node is Blueprint-callable"), Parse != nullptr && Parse->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	return true;
}
#endif
