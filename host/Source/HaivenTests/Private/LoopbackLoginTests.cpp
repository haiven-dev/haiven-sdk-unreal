// The loopback game login: the redirect rules, and a real round trip through the
// engine's HTTP server. The "browser" is FHttpModule and the exchange endpoint is
// a second loopback route, so the test proves the listener, the callback
// rebuild, FHandoffCore's checks and the form exchange together, with no
// network beyond 127.0.0.1 and no system browser.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "HaivenAsyncActions.h"
#include "HaivenLoopbackRedirect.h"
#include "HaivenSubsystem.h"
#include "HttpModule.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenLoopbackRedirectRules,
	"Haiven.Auth.LoopbackRedirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenLoopbackRedirectRules::RunTest(const FString& Parameters)
{
	using namespace Haiven::LoopbackRedirect;
	FEndpoint Endpoint;
	FString Error;
	TestTrue(TEXT("IPv4 loopback with port and path"), Parse(TEXT("http://127.0.0.1:47811/haiven/callback"), Endpoint, Error));
	TestEqual(TEXT("port"), static_cast<int32>(Endpoint.Port), 47811);
	TestEqual(TEXT("path"), Endpoint.Path, FString(TEXT("/haiven/callback")));
	TestEqual(TEXT("base"), Endpoint.Base, FString(TEXT("http://127.0.0.1:47811/haiven/callback")));
	TestTrue(TEXT("localhost with a configured query"), Parse(TEXT("http://localhost:8080/cb?tenant=one"), Endpoint, Error));
	TestEqual(TEXT("the query is not part of the path"), Endpoint.Path, FString(TEXT("/cb")));
	TestEqual(TEXT("the query is not part of the base"), Endpoint.Base, FString(TEXT("http://localhost:8080/cb")));

	const TCHAR* Refused[] = {
		TEXT("https://127.0.0.1:47811/haiven/callback"),
		TEXT("http://localhost.attacker.example:47811/haiven/callback"),
		TEXT("http://192.168.1.2:47811/haiven/callback"),
		TEXT("http://127.0.0.1/haiven/callback"),
		TEXT("http://127.0.0.1:0/haiven/callback"),
		TEXT("http://127.0.0.1:65536/haiven/callback"),
		TEXT("http://127.0.0.1:4x/haiven/callback"),
		TEXT("http://127.0.0.1:47811"),
		TEXT("http://127.0.0.1:47811/"),
		TEXT("http://127.0.0.1:47811/haiven/callback/"),
		TEXT("http://127.0.0.1:47811/haiven/callback#done"),
		TEXT("http://127.0.0.1:47811?x=/y"),
		TEXT("example-game://haiven/callback"),
	};
	for (const TCHAR* Url : Refused)
	{
		TestFalse(FString::Printf(TEXT("refused: %s"), Url), Parse(Url, Endpoint, Error));
		TestFalse(TEXT("a refusal explains itself"), Error.IsEmpty());
	}

	Parse(TEXT("http://127.0.0.1:47811/haiven/callback"), Endpoint, Error);
	TMap<FString, FString> Query;
	Query.Add(TEXT("state"), TEXT("abc_-"));
	Query.Add(TEXT("code"), TEXT("one time/+&="));
	Query.Add(TEXT("tenant"), TEXT("one"));
	TestEqual(TEXT("the callback is rebuilt sorted and re-encoded"),
		CallbackUrl(Endpoint, Query),
		FString(TEXT("http://127.0.0.1:47811/haiven/callback?code=one%20time%2F%2B%26%3D&state=abc_-&tenant=one")));
	TestEqual(TEXT("no query rebuilds the bare redirect"), CallbackUrl(Endpoint, {}), Endpoint.Base);

	const uint8 V4Loopback[] = { 127, 0, 0, 1 };
	const uint8 V4Other[] = { 10, 0, 0, 1 };
	uint8 V6Loopback[16] = {};
	V6Loopback[15] = 1;
	uint8 V6Mapped[16] = {};
	V6Mapped[10] = 0xFF;
	V6Mapped[11] = 0xFF;
	V6Mapped[12] = 127;
	V6Mapped[15] = 1;
	uint8 V6Other[16] = {};
	V6Other[0] = 0xFE;
	V6Other[1] = 0x80;
	V6Other[15] = 1;
	TestTrue(TEXT("127.0.0.1 is loopback"), IsLoopbackPeer(V4Loopback));
	TestFalse(TEXT("10.0.0.1 is not"), IsLoopbackPeer(V4Other));
	TestTrue(TEXT("::1 is loopback"), IsLoopbackPeer(V6Loopback));
	TestTrue(TEXT("::ffff:127.0.0.1 is loopback"), IsLoopbackPeer(V6Mapped));
	TestFalse(TEXT("fe80::1 is not"), IsLoopbackPeer(V6Other));
	TestFalse(TEXT("an empty address is not"), IsLoopbackPeer(TConstArrayView<uint8>()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenLoopbackLogin,
	"Haiven.Auth.LoopbackLogin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace HaivenLoopbackLoginDetail
{
	const TCHAR* const ExchangePath = TEXT("/v0/auth/handoff/exchange");

	/**
	 * A loopback port nothing holds right now, probed with a plain socket so a
	 * busy one never reaches the engine listener (whose bind failure logs an
	 * error). Below the ephemeral ranges, so this test's own outgoing
	 * connections cannot take it between the probe and the bind.
	 */
	uint32 FreeLoopbackPort(uint32 After)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		for (uint32 Port = After + 1; Sockets && Port < 32000; ++Port)
		{
			FSocket* Probe = Sockets->CreateSocket(NAME_Stream, TEXT("HaivenLoopbackProbe"), false);
			if (!Probe) return 0;
			TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
			Address->SetLoopbackAddress();
			Address->SetPort(static_cast<int32>(Port));
			const bool bFree = Probe->Bind(*Address);
			Probe->Close();
			Sockets->DestroySocket(Probe);
			if (bFree) return Port;
		}
		return 0;
	}

	uint32 RedirectPort = 0;
	uint32 ExchangePort = 0;
	uint32 DeadlinePort = 0;

	FString QueryValue(const FString& Url, const FString& Name)
	{
		FString Query;
		if (!Url.Split(TEXT("?"), nullptr, &Query)) return FString();
		TArray<FString> Pairs;
		Query.ParseIntoArray(Pairs, TEXT("&"));
		for (const FString& Pair : Pairs)
		{
			FString Key;
			FString Value;
			if (Pair.Split(TEXT("="), &Key, &Value) && Key == Name) return Value;
		}
		return FString();
	}

	FHaivenGameLoginOptions Options(uint32 Port, float Lifetime = 60.0f)
	{
		FHaivenGameLoginOptions Value;
		Value.EntryUrl = TEXT("https://app.example.test/connect/game");
		Value.ExchangeUrl = FString::Printf(TEXT("http://127.0.0.1:%u%s"), ExchangePort, ExchangePath);
		Value.IntegrationId = TEXT("loopback-test");
		Value.RedirectUrl = FString::Printf(TEXT("http://127.0.0.1:%u/haiven/loopback-test"), Port);
		Value.TransactionLifetimeSeconds = Lifetime;
		return Value;
	}

	struct FBrowserReply
	{
		bool bDone = false;
		int32 Status = 0;
		FString CacheControl;
	};

	/** What the system browser would do: GET the redirect it was sent to. */
	void Browse(const FString& Url, const TSharedRef<FBrowserReply>& Reply)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->OnProcessRequestComplete().BindLambda(
			[Reply](FHttpRequestPtr, FHttpResponsePtr Response, bool)
			{
				Reply->Status = Response.IsValid() ? Response->GetResponseCode() : -1;
				Reply->CacheControl = Response.IsValid() ? Response->GetHeader(TEXT("Cache-Control")) : FString();
				Reply->bDone = true;
			});
		Request->ProcessRequest();
	}

	struct FRun
	{
		TStrongObjectPtr<UGameInstance> GameInstance;
		TStrongObjectPtr<UHaivenSubsystem> Subsystem;
		TStrongObjectPtr<UHaivenSubsystem> DeadlineSubsystem;
		TSharedPtr<IHttpRouter> ExchangeRouter;
		FHttpRouteHandle ExchangeRoute;
		int32 Exchanges = 0;
		FString ExchangeBody;
		FString State;
		TSharedRef<FBrowserReply> Stray = MakeShared<FBrowserReply>();
		TSharedRef<FBrowserReply> Callback = MakeShared<FBrowserReply>();
		TSharedRef<FBrowserReply> Late = MakeShared<FBrowserReply>();
		int32 Phase = 0;
		double Deadline = 0.0;

		~FRun()
		{
			if (ExchangeRouter.IsValid() && ExchangeRoute.IsValid()) ExchangeRouter->UnbindRoute(ExchangeRoute);
			if (Subsystem) Subsystem->Logout();
			if (DeadlineSubsystem) DeadlineSubsystem->Logout();
		}
	};
}

bool FHaivenLoopbackLogin::RunTest(const FString& Parameters)
{
	using namespace HaivenLoopbackLoginDetail;
	RedirectPort = FreeLoopbackPort(28900);
	ExchangePort = FreeLoopbackPort(RedirectPort);
	DeadlinePort = FreeLoopbackPort(ExchangePort);
	if (!TestTrue(TEXT("three free loopback ports"), RedirectPort && ExchangePort && DeadlinePort)) return false;
	TSharedRef<FRun> Run = MakeShared<FRun>();
	Run->GameInstance.Reset(NewObject<UGameInstance>());
	Run->Subsystem.Reset(NewObject<UHaivenSubsystem>(Run->GameInstance.Get()));
	UHaivenSubsystem* Subsystem = Run->Subsystem.Get();
	int32 Launches = 0;
	FString Launched;
	Subsystem->SetLoginUrlLauncher([&Launches, &Launched](const FString& Url, FString&)
	{
		++Launches;
		Launched = Url;
		return true;
	});

	// Refusals that never open the browser or a port.
	FString Url;
	FString Error;
	FHaivenGameLoginOptions NotLoopback = Options(RedirectPort);
	NotLoopback.RedirectUrl = TEXT("example-game://haiven/callback");
	TestFalse(TEXT("a non-loopback redirect is not received by the game"), Subsystem->BeginLoopbackGameLogin(NotLoopback, Url, Error));
	TestFalse(TEXT("the refusal explains itself"), Error.IsEmpty());
	TestEqual(TEXT("a refused redirect never opens the browser"), Launches, 0);
	TestFalse(TEXT("a refused redirect leaves no transaction"), Subsystem->IsGameLoginInProgress());

	Subsystem->SetLoginUrlLauncher([](const FString&, FString& LaunchError)
	{
		LaunchError = TEXT("no browser in this test");
		return false;
	});
	TestFalse(TEXT("a failed launch fails the login"), Subsystem->BeginLoopbackGameLogin(Options(RedirectPort), Url, Error));
	TestFalse(TEXT("a failed launch releases the route"), Subsystem->LoopbackReceiver.IsValid());
	TestFalse(TEXT("a failed launch leaves no transaction"), Subsystem->IsGameLoginInProgress());

	Subsystem->SetLoginUrlLauncher([&Launches, &Launched](const FString& LaunchedUrl, FString&)
	{
		++Launches;
		Launched = LaunchedUrl;
		return true;
	});

	// The fake exchange endpoint: the contract's response, with no refresh credential.
	FHttpServerModule::Get().StartAllListeners();
	Run->ExchangeRouter = FHttpServerModule::Get().GetHttpRouter(ExchangePort, true);
	if (!TestTrue(TEXT("the fake exchange endpoint listens"), Run->ExchangeRouter.IsValid())) return false;
	TWeakPtr<FRun> WeakRun = Run;
	Run->ExchangeRoute = Run->ExchangeRouter->BindRoute(
		FHttpPath(ExchangePath),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([WeakRun](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			if (const TSharedPtr<FRun> Pinned = WeakRun.Pin())
			{
				++Pinned->Exchanges;
				FUTF8ToTCHAR Body(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
				Pinned->ExchangeBody = FString(Body.Length(), Body.Get());
			}
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(
				FString(TEXT(R"({"access_token":"loopback-access","token_type":"Bearer","expires_in":1800})")),
				TEXT("application/json"));
			Response->Code = EHttpServerResponseCodes::Ok;
			OnComplete(MoveTemp(Response));
			return true;
		}));
	if (!TestTrue(TEXT("the fake exchange route binds"), Run->ExchangeRoute.IsValid())) return false;

	TestTrue(TEXT("a loopback login starts"), Subsystem->BeginLoopbackGameLogin(Options(RedirectPort), Url, Error));
	TestEqual(TEXT("the browser opens once"), Launches, 1);
	TestEqual(TEXT("the launcher gets the authorization URL"), Launched, Url);
	TestEqual(TEXT("the registered redirect is sent exactly"),
		QueryValue(Url, TEXT("redirect_uri")),
		FString::Printf(TEXT("http%%3A%%2F%%2F127.0.0.1%%3A%u%%2Fhaiven%%2Floopback-test"), RedirectPort));
	TestTrue(TEXT("the login is in progress"), Subsystem->IsGameLoginInProgress());
	Run->State = QueryValue(Url, TEXT("state"));
	TestEqual(TEXT("the state is 256 bits"), Run->State.Len(), 43);

	// A second login whose browser never comes back.
	Run->DeadlineSubsystem.Reset(NewObject<UHaivenSubsystem>(Run->GameInstance.Get()));
	Run->DeadlineSubsystem->SetLoginUrlLauncher([](const FString&, FString&) { return true; });
	TestTrue(TEXT("a login with a one-second lifetime starts"),
		Run->DeadlineSubsystem->BeginLoopbackGameLogin(Options(DeadlinePort, 1.0f), Url, Error));

	const FString Redirect = FString::Printf(TEXT("http://127.0.0.1:%u/haiven/loopback-test"), RedirectPort);
	Run->Deadline = FPlatformTime::Seconds() + 20.0;
	Browse(Redirect + TEXT("?code=forged&state=not-this-login"), Run->Stray);

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Run, Redirect]() -> bool
	{
		UHaivenSubsystem* Owner = Run->Subsystem.Get();
		if (FPlatformTime::Seconds() > Run->Deadline)
		{
			AddError(FString::Printf(TEXT("the loopback round trip did not finish (phase %d)"), Run->Phase));
			return true;
		}
		switch (Run->Phase)
		{
		case 0:
			if (!Run->Stray->bDone) return false;
			TestEqual(TEXT("a request with another state is refused"), Run->Stray->Status, 400);
			TestTrue(TEXT("a refused request does not end the login"), Owner->IsGameLoginInProgress());
			TestEqual(TEXT("a refused request is never exchanged"), Run->Exchanges, 0);
			Browse(Redirect + TEXT("?code=one-time&state=") + Run->State, Run->Callback);
			Run->Phase = 1;
			return false;
		case 1:
		{
			FString Token;
			if (!Run->Callback->bDone || !Owner->GetAccessToken(Token)) return false;
			TestEqual(TEXT("the browser is told the game got the redirect"), Run->Callback->Status, 200);
			TestEqual(TEXT("the redirect page is never cached"), Run->Callback->CacheControl, FString(TEXT("no-store")));
			TestEqual(TEXT("the code is exchanged once"), Run->Exchanges, 1);
			TestTrue(TEXT("the exchange carries the code"), Run->ExchangeBody.Contains(TEXT("code=one-time&")));
			TestTrue(TEXT("the exchange carries the verifier"), Run->ExchangeBody.Contains(TEXT("code_verifier=")));
			TestTrue(TEXT("the exchange carries the integration"), Run->ExchangeBody.Contains(TEXT("integration_id=loopback-test")));
			TestTrue(TEXT("the exchange carries the exact redirect"), Run->ExchangeBody.EndsWith(
				FString::Printf(TEXT("redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A%u%%2Fhaiven%%2Floopback-test"), RedirectPort)));
			TestTrue(TEXT("the exchanged bearer is the subsystem's"), Token == TEXT("loopback-access"));
			const FHaivenHealth Health = Owner->GetHealth();
			const double Lifetime = (Health.AccessTokenExpiresAtUtc - FDateTime::UtcNow()).GetTotalSeconds();
			TestTrue(TEXT("the expiry is the exchange's expires_in, not an assumed hour"), Lifetime > 1700.0 && Lifetime <= 1800.0);
			TestFalse(TEXT("the login is over"), Owner->IsGameLoginInProgress());
			TestFalse(TEXT("the route is released"), Owner->LoopbackReceiver.IsValid());
			Browse(Redirect + TEXT("?code=one-time&state=") + Run->State, Run->Late);
			Run->Phase = 2;
			return false;
		}
		case 2:
		{
			if (!Run->Late->bDone || Run->DeadlineSubsystem->IsGameLoginInProgress()) return false;
			TestEqual(TEXT("a replayed redirect finds no route"), Run->Late->Status, 404);
			TestEqual(TEXT("a replay is never exchanged"), Run->Exchanges, 1);
			TestFalse(TEXT("an unanswered login releases its route at the deadline"),
				Run->DeadlineSubsystem->LoopbackReceiver.IsValid());
			return true;
		}
		default:
			return true;
		}
	}));
	// The deadline's failure broadcast is queued to a later game-thread turn.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Run]() -> bool
	{
		const bool bReported = Run->DeadlineSubsystem->GetHealth().LastError.Contains(TEXT("not completed in time"));
		if (!bReported && FPlatformTime::Seconds() <= Run->Deadline) return false;
		TestTrue(TEXT("the deadline reports why the login ended"), bReported);
		return true;
	}));
	return true;
}
#endif
