#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenRestApi.h"

// The generated request builders under the engine's own compiler and warning
// set. sdks/cpp/test/rest_api_test.cpp pins the same requests under plain C++17;
// this proves the header also builds inside an Unreal module.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenRestRequestBuilders,
	"Haiven.Rest.RequestBuilders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenRestRequestBuilders::RunTest(const FString& Parameters)
{
	const Haiven::Rest::FRestRequest Entitlements = Haiven::Rest::GetUsersMeEntitlements();
	TestEqual(TEXT("entitlement read is a GET"), FString(UTF8_TO_TCHAR(Entitlements.Method.c_str())), FString(TEXT("GET")));
	TestEqual(TEXT("of the account entitlements path"),
		FString(UTF8_TO_TCHAR(Entitlements.Path.c_str())), FString(TEXT("/users/me/entitlements")));

	const Haiven::Rest::FRestRequest Messages =
		Haiven::Rest::GetChatChannelsChannelIdMessages("chan/1 x", std::string("m&2"), std::nullopt, 25, std::nullopt);
	TestEqual(TEXT("path and query are percent-encoded"),
		FString(UTF8_TO_TCHAR(Messages.Path.c_str())),
		FString(TEXT("/chat/channels/chan%2F1%20x/messages?beforeMessageId=m%262&limit=25")));

	const Haiven::Rest::FRestRequest Sso =
		Haiven::Rest::GetAuthSsoProviderUrl("google", "challenge", "S256");
	TestTrue(TEXT("SSO initiation is a browser navigation"), Sso.bNavigation);
	return true;
}
#endif
