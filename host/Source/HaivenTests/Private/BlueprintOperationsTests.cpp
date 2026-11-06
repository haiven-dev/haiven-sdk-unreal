#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Blueprint/HaivenBlueprintRealtime.h"
#include "Blueprint/HaivenBlueprintRest.h"
#include "Engine/GameInstance.h"
#include "HaivenAsyncActions.h"

// The generated operation nodes (codegen/emit_blueprint_ops.py): the REST
// request builders, the send node's refusals, the typed event router, and the
// WebSocket request nodes' reflected shape. The builders are the C++ SDK's,
// tested in rest_api_test.cpp; this proves the Blueprint layer passes its pins
// through and routes frames to the right typed event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenBlueprintOperations,
	"Haiven.Blueprint.Operations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenBlueprintOperations::RunTest(const FString& Parameters)
{
	const FHaivenRestRequest App = UHaivenRestRequests::GetAppsAppId(TEXT("a b"));
	TestEqual(TEXT("method"), App.Method, FString(TEXT("GET")));
	TestEqual(TEXT("a path parameter is encoded"), App.Path, FString(TEXT("/apps/a%20b")));
	TestTrue(TEXT("a JSON request is sendable"), UHaivenRestRequestAsyncAction::Unsendable(App).IsEmpty());

	const FHaivenRestRequest Discovery = UHaivenRestRequests::GetChatCommunitiesDiscovery(false, TEXT("ignored"), true, 5);
	TestEqual(TEXT("only the present optional reaches the query"), Discovery.Path, FString(TEXT("/chat/communities/discovery?limit=5")));

	const FHaivenRestRequest Sso = UHaivenRestRequests::GetAuthSsoProviderUrl(
		TEXT("discord"), TEXT("challenge"), TEXT("S256"), false, TEXT(""));
	TestTrue(TEXT("a navigation is marked"), Sso.bNavigation);
	TestFalse(TEXT("and refused by the send node"), UHaivenRestRequestAsyncAction::Unsendable(Sso).IsEmpty());
	TestFalse(TEXT("a form body is refused by the send node"),
		UHaivenRestRequestAsyncAction::Unsendable(UHaivenRestRequests::PostAuthHandoffExchange(TEXT("code=x"))).IsEmpty());

	// A game-instance subsystem must live inside a game instance (its ClassWithin);
	// Route needs nothing else, so it is not initialised against a session.
	UHaivenRealtimeEvents* Events = NewObject<UHaivenRealtimeEvents>(NewObject<UGameInstance>());
	TestTrue(TEXT("a known event with a valid payload is routed"), Events->Route(TEXT("motd"),
		TEXT(R"({"type":"motd","payload":{"id":"m-1","title":"Hi","body":"B","startsAt":"2026-01-01T00:00:00Z"}})")) == EHaivenRealtimeEventRoute::Routed);
	TestTrue(TEXT("a known event with a bad payload is invalid"), Events->Route(TEXT("motd"),
		TEXT(R"({"type":"motd","payload":{"id":"m-1"}})")) == EHaivenRealtimeEventRoute::Invalid);
	TestTrue(TEXT("a known event without a payload is invalid"), Events->Route(TEXT("motd"),
		TEXT(R"({"type":"motd"})")) == EHaivenRealtimeEventRoute::Invalid);
	TestTrue(TEXT("an event this build predates is passed through"), Events->Route(TEXT("chat.future"),
		TEXT(R"({"type":"chat.future","payload":{}})")) == EHaivenRealtimeEventRoute::Unrecognized);
	TestTrue(TEXT("an inbound chat message is decoded and routed to its typed event"),
		Events->Route(TEXT("chat.message"),
			TEXT(R"({"type":"chat.message","payload":{"channelId":"ch-1","id":"m-1","content":"hi"}})")) == EHaivenRealtimeEventRoute::Routed);
	TestTrue(TEXT("an inbound typing state is decoded and routed to its typed event"),
		Events->Route(TEXT("chat.typing"),
			TEXT(R"({"type":"chat.typing","payload":{"channelId":"ch-1","userId":"u-1","isTyping":true}})")) == EHaivenRealtimeEventRoute::Routed);
	TestTrue(TEXT("a malformed inbound chat message is rejected"), Events->Route(TEXT("chat.message"),
		TEXT(R"({"type":"chat.message","payload":{"id":"m-1"}})")) == EHaivenRealtimeEventRoute::Invalid);
	TestTrue(TEXT("a malformed inbound typing state is rejected"), Events->Route(TEXT("chat.typing"),
		TEXT(R"({"type":"chat.typing","payload":{"channelId":"ch-1"}})")) == EHaivenRealtimeEventRoute::Invalid);
	TestNotNull(TEXT("chat messages have a reflected typed delegate"),
		UHaivenRealtimeEvents::StaticClass()->FindPropertyByName(TEXT("OnChatMessage")));
	TestNotNull(TEXT("typing states have a reflected typed delegate"),
		UHaivenRealtimeEvents::StaticClass()->FindPropertyByName(TEXT("OnChatTyping")));

	const UFunction* Send = UHaivenChatSendAsyncAction::StaticClass()->FindFunctionByName(TEXT("RealtimeChatSend"));
	TestTrue(TEXT("a request node is Blueprint-callable"), Send != nullptr && Send->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	TestNotNull(TEXT("its typed payload pin"), Send ? Send->FindPropertyByName(TEXT("Payload")) : nullptr);
	TestNotNull(TEXT("its typed reply"), UHaivenChatSendAsyncAction::StaticClass()->FindPropertyByName(TEXT("Completed")));
	TestNotNull(TEXT("an unanswered request completes on send"), UHaivenChatTypingAsyncAction::StaticClass()->FindPropertyByName(TEXT("Sent")));
	return true;
}
#endif
