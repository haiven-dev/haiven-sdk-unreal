#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenWsApi.h"

// The generated WebSocket frame builders under the engine's own compiler and
// warning set. sdks/cpp/test/ws_api_test.cpp pins the same frames under plain
// C++17; this proves the header also builds inside an Unreal module.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenWsFrameBuilders,
	"Haiven.Ws.FrameBuilders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenWsFrameBuilders::RunTest(const FString& Parameters)
{
	const Haiven::Ws::FWsRequest Send = Haiven::Ws::ChatSend("{\"channelId\":\"c1\",\"content\":\"hi\"}");
	TestEqual(TEXT("chat.send carries its wire type"), FString(UTF8_TO_TCHAR(Send.Type.c_str())), FString(TEXT("chat.send")));
	TestEqual(TEXT("and names its reply contract"),
		FString(UTF8_TO_TCHAR(Send.ReplySchema.c_str())), FString(TEXT("ChatMessageSubmissionV0")));

	const Haiven::Ws::FWsEvent* Linked = Haiven::Ws::FindEvent("account.identity.linked");
	TestTrue(TEXT("the OAuth link event is routed"), Linked != nullptr);
	TestNotNull(TEXT("and so is the entitlement refresh hint"), Haiven::Ws::FindEvent("entitlements.updated"));
	TestNull(TEXT("an unknown event is unknown, not an error"), Haiven::Ws::FindEvent("entitlements.minted.later"));
	return true;
}
#endif
