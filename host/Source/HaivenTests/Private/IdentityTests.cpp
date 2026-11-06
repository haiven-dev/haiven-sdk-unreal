#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenIdentity.h"
#include "HaivenIdentityCore.h"
#include "HaivenSubsystem.h"
#include "Engine/GameInstance.h"
#include "Interfaces/IHttpRequest.h"

#include <string>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenIdentityBridge,
	"Haiven.Identity.Bridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenIdentityBridge::RunTest(const FString& Parameters)
{
	TArray<FString> Calls;
	Haiven::FIdentityCore Identity(
		[&Calls](std::string& Context, std::string&)
		{
			Calls.Add(TEXT("prepare"));
			Context = "popup";
			return true;
		},
		[&Calls](const std::string& Context, const std::string& Url, std::string&)
		{
			Calls.Add(FString::Printf(TEXT("post:%s:%s"), UTF8_TO_TCHAR(Context.c_str()), UTF8_TO_TCHAR(Url.c_str())));
			return true;
		},
		[&Calls](const std::string& Context)
		{
			Calls.Add(FString::Printf(TEXT("close:%s"), UTF8_TO_TCHAR(Context.c_str())));
		});

	std::string Error;
	TestTrue(TEXT("link preparation succeeds"), Identity.BeginLink("https://api.haiven.test/v0", "google", Error));
	TestEqual(TEXT("the browser context is prepared before navigation"), Calls[0], FString(TEXT("prepare")));
	TestEqual(TEXT("the exact same context performs the POST navigation"), Calls[1],
		FString(TEXT("post:popup:https://api.haiven.test/v0/users/me/sign-in-methods/sso/google")));

	const Haiven::FIdentityEvent Linked = Identity.HandleRealtimeEvent(
		Haiven::Ws::Types::AccountIdentityLinked,
		R"({"type":"account.identity.linked","payload":{"provider":"google","email":"player@example.test"}})");
	TestTrue(TEXT("strict identity event validation succeeds"), Linked.bValid && Linked.Linked.has_value());
	TestEqual(TEXT("the matching outcome closes its browser context"), Calls.Last(), FString(TEXT("close:popup")));

	bool bChanged = false;

	// Subsystem-level GET-before-DELETE overlap. Real requests are created against
	// a closed loopback port, then settled here in a controlled order; each
	// transport is detached first so its own completion cannot interleave.
	UHaivenSubsystem* UnlinkSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	UnlinkSubsystem->ResetIdentity();
	FHaivenClientOptions Options;
	Options.BaseUrl = TEXT("http://127.0.0.1:9");
	UnlinkSubsystem->Configure(Options);
	UnlinkSubsystem->SetAccessToken(TEXT("player-a"), FDateTime::UtcNow() + FTimespan::FromHours(1));
	auto Pending = [UnlinkSubsystem](const TCHAR* Verb)
	{
		TArray<FGuid> Handles;
		for (const auto& Pair : UnlinkSubsystem->HttpRequests)
		{
			if (Pair.Value.Request.IsValid() && Pair.Value.Request->GetVerb() == Verb) Handles.Add(Pair.Key);
		}
		return Handles;
	};
	auto Settle = [UnlinkSubsystem](const FGuid& Handle, int32 Status, const FString& Body)
	{
		if (const auto* Existing = UnlinkSubsystem->HttpRequests.Find(Handle))
		{
			if (Existing->Request.IsValid())
			{
				Existing->Request->OnProcessRequestComplete().Unbind();
				Existing->Request->CancelRequest();
			}
		}
		FHaivenHttpResult Result;
		Result.StatusCode = Status;
		Result.bSucceeded = Status >= 200 && Status < 300;
		Result.Body = Body;
		UnlinkSubsystem->CompleteHttpRequest(Handle, UnlinkSubsystem->CredentialGeneration, Result);
	};
	UnlinkSubsystem->RefreshSignInMethods();
	UnlinkSubsystem->UnlinkIdentity(TEXT("google"));
	const TArray<FGuid> OverlappingGet = Pending(TEXT("GET"));
	const TArray<FGuid> UnlinkDelete = Pending(TEXT("DELETE"));
	if (!TestEqual(TEXT("one sign-in-method GET is in flight"), OverlappingGet.Num(), 1)
		|| !TestEqual(TEXT("one unlink DELETE is in flight"), UnlinkDelete.Num(), 1))
	{
		UnlinkSubsystem->Logout();
		return false;
	}
	Settle(OverlappingGet[0], 200,
		TEXT(R"({"hasPassword":true,"identities":[{"provider":"google","email":"still-linked@example.test"}]})"));
	FHaivenSignInMethods Methods;
	TestTrue(TEXT("the pre-unlink GET publishes the still-linked provider"),
		UnlinkSubsystem->GetCurrentSignInMethods(Methods) && Methods.Identities.Num() == 1);
	Settle(UnlinkDelete[0], 204, FString());
	const TArray<FGuid> FollowUpGet = Pending(TEXT("GET"));
	if (!TestEqual(TEXT("a successful unlink issues one follow-up GET despite the completed read"),
		FollowUpGet.Num(), 1))
	{
		UnlinkSubsystem->Logout();
		return false;
	}
	Settle(FollowUpGet[0], 200, TEXT(R"({"hasPassword":true,"identities":[]})"));
	TestTrue(TEXT("the follow-up list no longer reports Google"),
		UnlinkSubsystem->GetCurrentSignInMethods(Methods) && Methods.Identities.IsEmpty());

	UnlinkSubsystem->UnlinkIdentity(TEXT("google"));
	const TArray<FGuid> StaleDelete = Pending(TEXT("DELETE"));
	UnlinkSubsystem->Logout();
	TestTrue(TEXT("an account change still cancels the unlink completion"),
		StaleDelete.Num() == 1 && Pending(TEXT("GET")).IsEmpty() && Pending(TEXT("DELETE")).IsEmpty());

	// The public MOTD read is not the account's: a token set while it is in
	// flight (a launch token applied after the pre-sign-in read) must not drop it.
	UnlinkSubsystem->LoadMotd(TEXT("game-1"));
	const TArray<FGuid> PublicMotd = Pending(TEXT("GET"));
	UnlinkSubsystem->SetAccessToken(TEXT("player-b"), FDateTime::UtcNow() + FTimespan::FromHours(1));
	if (!TestEqual(TEXT("one public MOTD read is in flight"), PublicMotd.Num(), 1)
		|| !TestTrue(TEXT("the public MOTD read survives the account boundary"), Pending(TEXT("GET")) == PublicMotd))
	{
		UnlinkSubsystem->Logout();
		return false;
	}
	Settle(PublicMotd[0], 200,
		TEXT(R"({"id":"33333333-3333-4333-8333-333333333333","body":"Before sign-in","startsAt":"2026-10-14T00:00:00Z"})"));
	FHaivenMotd Visible;
	TestTrue(TEXT("the MOTD read before the token was set still lands"),
		UnlinkSubsystem->GetCurrentMotd(Visible) && Visible.Body == TEXT("Before sign-in"));
	UnlinkSubsystem->Logout();

	const auto MotdTicket = Identity.BeginMotdRead();
	TestTrue(TEXT("a public message parses"), Identity.CompleteMotdRead(
		MotdTicket,
		200,
		R"({"id":"11111111-1111-4111-8111-111111111111","body":"Hello","url":"https://haiven.gg/news","startsAt":"2026-10-14T00:00:00Z"})",
		bChanged,
		Error));
	TestTrue(TEXT("the parsed message is initially visible"), bChanged && Identity.GetVisibleMotd() != nullptr);
	TestTrue(TEXT("the current message can be dismissed"), Identity.DismissMotd());
	TestNull(TEXT("dismissal suppresses the current id"), Identity.GetVisibleMotd());
	TestFalse(TEXT("the same id cannot be dismissed twice"), Identity.DismissMotd());
	const Haiven::FIdentityEvent NewMotd = Identity.HandleRealtimeEvent(
		Haiven::Ws::Types::Motd,
		R"({"type":"motd","payload":{"id":"22222222-2222-4222-8222-222222222222","body":"New","startsAt":"2026-10-15T00:00:00Z"}})");
	TestTrue(TEXT("a new id is visible after dismissal"),
		NewMotd.bValid && NewMotd.bChanged && Identity.GetVisibleMotd() != nullptr);
	TestTrue(TEXT("absolute HTTPS actions are accepted"),
		UHaivenSubsystem::IsValidMotdActionUrl(TEXT("https://haiven.gg/news")));
	TestFalse(TEXT("HTTP actions are rejected"),
		UHaivenSubsystem::IsValidMotdActionUrl(TEXT("http://haiven.gg/news")));
	TestFalse(TEXT("relative actions are rejected"),
		UHaivenSubsystem::IsValidMotdActionUrl(TEXT("/news")));

	UClass* Subsystem = UHaivenSubsystem::StaticClass();
	TestNotNull(TEXT("Begin Identity Link is reflected for Blueprints"), Subsystem->FindFunctionByName(TEXT("BeginIdentityLink")));
	TestNotNull(TEXT("Load MOTD is reflected for Blueprints"), Subsystem->FindFunctionByName(TEXT("LoadMotd")));
	TestNotNull(TEXT("Dismiss MOTD is reflected for Blueprints"), Subsystem->FindFunctionByName(TEXT("DismissMotd")));
	TestNotNull(TEXT("MOTD action validation is reflected for Blueprints"), Subsystem->FindFunctionByName(TEXT("IsValidMotdActionUrl")));
	TestNotNull(TEXT("Refresh Sign-In Methods is reflected for Blueprints"), Subsystem->FindFunctionByName(TEXT("RefreshSignInMethods")));
	TestNotNull(TEXT("Unlink Identity is reflected for Blueprints"), Subsystem->FindFunctionByName(TEXT("UnlinkIdentity")));
	TestTrue(TEXT("the injected browser adapter is abstract"), UHaivenBrowserContextAdapter::StaticClass()->HasAnyClassFlags(CLASS_Abstract));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenIdentityMotdOrdering,
	"Haiven.Identity.MotdOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenIdentityMotdOrdering::RunTest(const FString& Parameters)
{
	Haiven::FIdentityCore Identity;
	bool bChanged = false;
	std::string Error;
	const auto Older = Identity.BeginMotdRead();
	const auto Newer = Identity.BeginMotdRead();
	TestFalse(TEXT("a newer same-domain request invalidates the older ticket"), Identity.IsCurrent(Older));
	TestTrue(TEXT("the newest ticket is current"), Identity.IsCurrent(Newer));
	TestTrue(TEXT("the newest response is accepted"), Identity.CompleteMotdRead(
		Newer,
		200,
		R"({"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","body":"A","startsAt":"2026-10-14T00:00:00Z"})",
		bChanged,
		Error));
	TestFalse(TEXT("a terminal response invalidates its own ticket"), Identity.IsCurrent(Newer));
	TestFalse(TEXT("the stale older response cannot overwrite the terminal result"), Identity.CompleteMotdRead(
		Older,
		200,
		R"({"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","body":"stale","startsAt":"2026-10-13T00:00:00Z"})",
		bChanged,
		Error));
	TestEqual(TEXT("the newest payload remains authoritative"),
		FString(UTF8_TO_TCHAR(Identity.GetMotd()->Id.c_str())),
		FString(TEXT("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")));

	TestTrue(TEXT("the visible id can be dismissed"), Identity.DismissMotd());
	const auto PendingAfterDismissal = Identity.BeginMotdRead();
	const Haiven::FIdentityEvent Suppressed = Identity.HandleRealtimeEvent(
		Haiven::Ws::Types::Motd,
		R"({"type":"motd","payload":{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","body":"changed duplicate","startsAt":"2026-10-15T00:00:00Z"}})");
	TestTrue(TEXT("a dismissed id remains a valid suppressed event"), Suppressed.bValid);
	TestFalse(TEXT("a dismissed id is rejected by id before assignment"), Suppressed.bChanged);
	TestTrue(TEXT("a suppressed event does not invalidate an in-flight REST read"),
		Identity.IsCurrent(PendingAfterDismissal));

	TestTrue(TEXT("an unsafe optional action does not reject its message"), Identity.CompleteMotdRead(
		PendingAfterDismissal,
		200,
		R"({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","body":"safe body","url":"https://user@example.test/private","startsAt":"2026-10-16T00:00:00Z"})",
		bChanged,
		Error));
	TestTrue(TEXT("the unsafe optional action is stripped"),
		Identity.GetMotd().has_value() && !Identity.GetMotd()->Url.has_value());

	const auto FailedRead = Identity.BeginMotdRead();
	TestFalse(TEXT("a transport failure is terminal"),
		Identity.CompleteMotdRead(FailedRead, 503, "", bChanged, Error));
	const Haiven::FIdentityEvent Recovery = Identity.HandleRealtimeEvent(
		Haiven::Ws::Types::Motd,
		R"({"type":"motd","payload":{"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","body":"safe body","startsAt":"2026-10-16T00:00:00Z"}})");
	TestTrue(TEXT("same-id realtime delivery recovers a failed presentation"),
		Recovery.bValid && Recovery.bChanged);
	return true;
}
#endif
