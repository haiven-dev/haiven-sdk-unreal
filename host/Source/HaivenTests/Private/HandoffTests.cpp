#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenHandoff.h"

namespace
{
	Haiven::FHandoffConfig Config()
	{
		Haiven::FHandoffConfig Value;
		Value.EntryUrl = TEXT("https://id.haiven.gg/game-login");
		Value.ExchangeUrl = TEXT("https://api.haiven.gg/v0/auth/handoff/exchange");
		Value.IntegrationId = TEXT("unreal-test");
		Value.RedirectUrl = TEXT("haiven://login/complete?tenant=one");
		Value.InstructionsId = TEXT("controller");
		Value.ScreenHint = TEXT("signup");
		Value.TransactionLifetime = FTimespan::FromSeconds(60);
		return Value;
	}

	TArray<uint8> Entropy(uint8 Seed)
	{
		TArray<uint8> Bytes;
		for (int32 Index = 0; Index < 32; ++Index) Bytes.Add(Seed + Index);
		return Bytes;
	}

	bool Begin(Haiven::FHandoffCore& Core, const FDateTime& Now, FString& Url, FString& State, FString& Error)
	{
		return Core.Begin(Config(), Entropy(0), Entropy(32), Now, Url, State, Error);
	}

	/** Saves across a simulated restart: a second FHandoffCore over it is the next launch. */
	class FSavedCredentialStore final : public Haiven::IHandoffCredentialStore
	{
	public:
		Haiven::FHandoffCredential Saved;
		bool bSaved = false;

		virtual bool Load(Haiven::FHandoffCredential& OutCredential) override
		{
			if (!bSaved) return false;
			OutCredential = Saved;
			return true;
		}
		virtual void Store(const Haiven::FHandoffCredential& Credential) override
		{
			Saved = Credential;
			bSaved = true;
		}
		virtual void Clear() override
		{
			Saved = Haiven::FHandoffCredential();
			bSaved = false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenHandoffPkce,
	"Haiven.Auth.PkceAndAuthorizationUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenHandoffPkce::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("RFC 7636 S256 vector"),
		Haiven::FHandoffCore::PkceChallenge(TEXT("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
		FString(TEXT("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")));

	Haiven::FHandoffCore Core;
	FString Url;
	FString State;
	FString Error;
	TestTrue(TEXT("begin succeeds"), Begin(Core, FDateTime(2026, 9, 17), Url, State, Error));
	TestEqual(TEXT("256-bit state encodes to 43 base64url characters"), State.Len(), 43);
	TestTrue(TEXT("authorization carries S256"), Url.Contains(TEXT("code_challenge_method=S256")));
	TestTrue(TEXT("authorization carries exact integration"), Url.Contains(TEXT("integration_id=unreal-test")));
	TestTrue(TEXT("authorization carries redirect completion"), Url.Contains(TEXT("completion=redirect")));
	TestFalse(TEXT("authorization never carries verifier"), Url.Contains(TEXT("code_verifier")));

	Haiven::FHandoffConfig Unsafe = Config();
	Unsafe.EntryUrl = TEXT("http://localhost.attacker.example/login");
	TestFalse(TEXT("lookalike loopback host is rejected"), Core.Begin(
		Unsafe, Entropy(0), Entropy(32), FDateTime(2026, 9, 17), Url, State, Error));

	Haiven::FHandoffConfig ExistingQuery = Config();
	ExistingQuery.EntryUrl = TEXT("https://id.haiven.gg/game-login?state=attacker&integration_id=wrong&theme=dark");
	TestTrue(TEXT("begin replaces reserved entry parameters"), Core.Begin(
		ExistingQuery, Entropy(0), Entropy(32), FDateTime(2026, 9, 17), Url, State, Error));
	TestFalse(TEXT("untrusted state is replaced"), Url.Contains(TEXT("state=attacker")));
	TestFalse(TEXT("untrusted integration is replaced"), Url.Contains(TEXT("integration_id=wrong")));
	TestTrue(TEXT("unrelated entry query is retained"), Url.Contains(TEXT("theme=dark")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenHandoffCallbackGuards,
	"Haiven.Auth.CallbackGuardsAndForm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenHandoffCallbackGuards::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 9, 17);
	Haiven::FHandoffCore Core;
	FString Url;
	FString State;
	FString Error;
	TestTrue(TEXT("begin succeeds"), Begin(Core, Now, Url, State, Error));

	Haiven::FHandoffExchange Exchange;
	TestFalse(TEXT("redirect mismatch is rejected"), Core.ConsumeCallback(
		TEXT("haiven://attacker/complete?tenant=one&state=") + State + TEXT("&code=bad"), Now, Exchange, Error));
	TestTrue(TEXT("redirect mismatch does not consume"), Core.HasPendingTransaction());
	TestFalse(TEXT("state mismatch is rejected"), Core.ConsumeCallback(
		TEXT("haiven://login/complete?tenant=one&state=wrong&code=bad"), Now, Exchange, Error));
	TestTrue(TEXT("state mismatch does not consume"), Core.HasPendingTransaction());
	TestTrue(TEXT("bound callback succeeds"), Core.ConsumeCallback(
		TEXT("haiven://login/complete?tenant=one&state=") + State + TEXT("&code=one%20time"), Now, Exchange, Error));
	TestEqual(TEXT("exchange endpoint is retained"), Exchange.ExchangeUrl, Config().ExchangeUrl);
	TestEqual(
		TEXT("form has exactly the contract fields"),
		Exchange.ToFormBody(),
		FString(TEXT("code=one%20time&code_verifier=ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8&integration_id=unreal-test&redirect_uri=haiven%3A%2F%2Flogin%2Fcomplete%3Ftenant%3Done")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenHandoffLifetime,
	"Haiven.Auth.ReplayExpiryCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenHandoffLifetime::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 9, 17);
	Haiven::FHandoffCore Core;
	FString Url;
	FString State;
	FString Error;
	Haiven::FHandoffExchange Exchange;

	TestTrue(TEXT("begin for replay"), Begin(Core, Now, Url, State, Error));
	const FString Callback = TEXT("haiven://login/complete?tenant=one&state=") + State + TEXT("&code=once");
	TestTrue(TEXT("first redemption succeeds"), Core.ConsumeCallback(Callback, Now, Exchange, Error));
	TestFalse(TEXT("replay fails"), Core.ConsumeCallback(Callback, Now, Exchange, Error));

	TestTrue(TEXT("begin for expiry"), Begin(Core, Now, Url, State, Error));
	TestFalse(TEXT("expired transaction fails"), Core.ConsumeCallback(
		TEXT("haiven://login/complete?tenant=one&state=") + State + TEXT("&code=late"),
		Now + FTimespan::FromSeconds(61), Exchange, Error));
	TestFalse(TEXT("expired transaction is consumed"), Core.HasPendingTransaction());

	TestTrue(TEXT("begin for cancellation"), Begin(Core, Now, Url, State, Error));
	Core.Cancel();
	TestFalse(TEXT("cancel clears verifier state"), Core.HasPendingTransaction());
	TestFalse(TEXT("cancelled callback fails"), Core.ConsumeCallback(
		TEXT("haiven://login/complete?tenant=one&state=") + State + TEXT("&code=cancelled"), Now, Exchange, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenHandoffCredential,
	"Haiven.Auth.CredentialLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenHandoffCredential::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 9, 17);
	Haiven::FHandoffCore Core;
	Haiven::FHandoffCredential Credential;
	FString Error;
	TestTrue(TEXT("credential parses"), Core.StoreExchangeResponse(
		TEXT("{\"access_token\":\"access-1\",\"token_type\":\"Bearer\",\"expires_in\":30}"),
		Now, Credential, Error));
	TestEqual(TEXT("absolute expiry is owned"), Credential.ExpiresAt, Now + FTimespan::FromSeconds(30));
	FString Token;
	TestTrue(TEXT("valid token is readable"), Core.GetAccessToken(Now + FTimespan::FromSeconds(29), Token));
	TestEqual(TEXT("token value"), Token, FString(TEXT("access-1")));
	TestFalse(TEXT("expired token is not readable"), Core.GetAccessToken(Now + FTimespan::FromSeconds(30), Token));
	TestFalse(TEXT("refresh credential is forbidden"), Core.StoreExchangeResponse(
		TEXT("{\"access_token\":\"access-2\",\"token_type\":\"Bearer\",\"expires_in\":30,\"refresh_token\":\"never\"}"),
		Now, Credential, Error));
	TestFalse(TEXT("a missing expires_in is refused"), Haiven::FHandoffCore().StoreExchangeResponse(
		TEXT("{\"access_token\":\"a\",\"token_type\":\"Bearer\"}"),
		Now, Credential, Error));
	TestFalse(TEXT("a string expires_in is refused"), Haiven::FHandoffCore().StoreExchangeResponse(
		TEXT("{\"access_token\":\"a\",\"token_type\":\"Bearer\",\"expires_in\":\"30\"}"),
		Now, Credential, Error));
	TestFalse(TEXT("a fractional expires_in is refused"), Haiven::FHandoffCore().StoreExchangeResponse(
		TEXT("{\"access_token\":\"a\",\"token_type\":\"Bearer\",\"expires_in\":1.5}"),
		Now, Credential, Error));
	TestFalse(TEXT("a negative expires_in is refused"), Haiven::FHandoffCore().StoreExchangeResponse(
		TEXT("{\"access_token\":\"a\",\"token_type\":\"Bearer\",\"expires_in\":-1}"),
		Now, Credential, Error));
	Core.ClearCredential();
	TestFalse(TEXT("logout clears token"), Core.GetAccessToken(Now, Token));

	// ADR 0041: a null expires_in is a durable credential, and a store lets the
	// next launch reuse it without a browser handoff.
	const TSharedPtr<FSavedCredentialStore> Store = MakeShared<FSavedCredentialStore>();
	{
		Haiven::FHandoffCore Durable;
		Durable.SetCredentialStore(Store);
		TestTrue(TEXT("a null expires_in parses"), Durable.StoreExchangeResponse(
			TEXT("{\"access_token\":\"durable\",\"token_type\":\"Bearer\",\"expires_in\":null}"),
			Now, Credential, Error));
		TestFalse(TEXT("a durable credential has no expiry"), Credential.bHasExpiry);
		TestTrue(TEXT("a durable credential is readable far ahead"), Durable.GetAccessToken(Now + FTimespan::FromDays(36500), Token));
		TestTrue(TEXT("the store saved the durable credential"), Store->bSaved && !Store->Saved.bHasExpiry);
	}
	TestTrue(TEXT("shutting down keeps the saved credential"), Store->bSaved);
	Haiven::FHandoffCore Relaunch;
	Relaunch.SetCredentialStore(Store);
	TestTrue(TEXT("a saved credential is reused at start"), Relaunch.Resume(Now, Credential));
	TestEqual(TEXT("the reused credential"), Credential.AccessToken, FString(TEXT("durable")));
	TestFalse(TEXT("reuse leaves no pending handoff"), Relaunch.HasPendingTransaction());
	Relaunch.ClearCredential();
	TestFalse(TEXT("logout clears the saved credential"), Store->bSaved);
	Haiven::FHandoffCredential Expired;
	Expired.AccessToken = TEXT("finite");
	Expired.TokenType = TEXT("Bearer");
	Expired.ExpiresAt = Now;
	Store->Store(Expired);
	TestFalse(TEXT("an expired saved credential is not reused"), Relaunch.Resume(Now, Credential));
	TestFalse(TEXT("an expired saved credential is cleared"), Store->bSaved);
	return true;
}
#endif
