#include "HaivenHandoff.h"

#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
	FString Base64Url(TConstArrayView<uint8> Bytes)
	{
		FString Value = FBase64::Encode(Bytes.GetData(), Bytes.Num());
		Value.ReplaceInline(TEXT("+"), TEXT("-"));
		Value.ReplaceInline(TEXT("/"), TEXT("_"));
		while (Value.EndsWith(TEXT("="))) Value.LeftChopInline(1);
		return Value;
	}

	struct FParsedUrl
	{
		FString Scheme;
		FString Authority;
		FString Path;
		TMap<FString, FString> Query;
	};

	bool ParseQuery(const FString& Text, TMap<FString, FString>& Out)
	{
		TArray<FString> Fields;
		Text.ParseIntoArray(Fields, TEXT("&"), true);
		for (const FString& Field : Fields)
		{
			FString Key;
			FString Value;
			if (!Field.Split(TEXT("="), &Key, &Value))
			{
				Key = Field;
			}
			Key = FGenericPlatformHttp::UrlDecode(Key.Replace(TEXT("+"), TEXT(" ")));
			Value = FGenericPlatformHttp::UrlDecode(Value.Replace(TEXT("+"), TEXT(" ")));
			if (Key.IsEmpty() || Out.Contains(Key)) return false;
			Out.Add(MoveTemp(Key), MoveTemp(Value));
		}
		return true;
	}

	bool ParseAbsoluteUrl(const FString& Url, FParsedUrl& Out)
	{
		if (Url.Contains(TEXT("#"))) return false;
		int32 SchemeEnd = INDEX_NONE;
		if (!Url.FindChar(TEXT(':'), SchemeEnd) || SchemeEnd <= 0) return false;
		Out.Scheme = Url.Left(SchemeEnd).ToLower();
		int32 Cursor = SchemeEnd + 1;
		if (Url.Mid(Cursor, 2) == TEXT("//"))
		{
			Cursor += 2;
			int32 AuthorityEnd = Url.Len();
			for (const TCHAR Separator : {TEXT('/'), TEXT('?')})
			{
				const int32 Found = Url.Find(FString::Chr(Separator), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
				if (Found != INDEX_NONE) AuthorityEnd = FMath::Min(AuthorityEnd, Found);
			}
			Out.Authority = Url.Mid(Cursor, AuthorityEnd - Cursor).ToLower();
			if (Out.Authority.IsEmpty()) return false;
			Cursor = AuthorityEnd;
		}

		const int32 QueryStart = Url.Find(TEXT("?"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
		const int32 PathEnd = QueryStart == INDEX_NONE ? Url.Len() : QueryStart;
		Out.Path = Url.Mid(Cursor, PathEnd - Cursor);
		if (Out.Path.IsEmpty()) Out.Path = TEXT("/");
		return QueryStart == INDEX_NONE || ParseQuery(Url.Mid(QueryStart + 1), Out.Query);
	}

	FString Encode(const FString& Value)
	{
		return FGenericPlatformHttp::UrlEncode(Value);
	}

	FString BuildAuthorizationUrl(const Haiven::FHandoffConfig& Config, const FString& State, const FString& Challenge)
	{
		FParsedUrl Entry;
		ParseAbsoluteUrl(Config.EntryUrl, Entry);
		Entry.Query.Add(TEXT("integration_id"), Config.IntegrationId);
		Entry.Query.Add(TEXT("completion"), TEXT("redirect"));
		Entry.Query.Add(TEXT("redirect_uri"), Config.RedirectUrl);
		Entry.Query.Add(TEXT("state"), State);
		Entry.Query.Add(TEXT("code_challenge"), Challenge);
		Entry.Query.Add(TEXT("code_challenge_method"), TEXT("S256"));
		if (!Config.InstructionsId.IsEmpty()) Entry.Query.Add(TEXT("instructions_id"), Config.InstructionsId);
		if (!Config.ScreenHint.IsEmpty()) Entry.Query.Add(TEXT("screen_hint"), Config.ScreenHint);

		FString Result = Config.EntryUrl;
		int32 ExistingQuery = INDEX_NONE;
		if (Result.FindChar(TEXT('?'), ExistingQuery)) Result.LeftInline(ExistingQuery);
		FString Separator = TEXT("?");
		for (const TPair<FString, FString>& Pair : Entry.Query)
		{
			Result += Separator + Encode(Pair.Key) + TEXT("=") + Encode(Pair.Value);
			Separator = TEXT("&");
		}
		return Result;
	}

	void SecureReset(FString& Value)
	{
		if (Value.GetCharArray().Num() > 0)
		{
			FMemory::Memzero(Value.GetCharArray().GetData(), Value.GetCharArray().Num() * sizeof(TCHAR));
		}
		Value.Reset();
	}

	bool IsSecureWebUrl(const FParsedUrl& Url)
	{
		const bool bLoopback = Url.Authority == TEXT("localhost")
			|| Url.Authority.StartsWith(TEXT("localhost:"))
			|| Url.Authority == TEXT("127.0.0.1")
			|| Url.Authority.StartsWith(TEXT("127.0.0.1:"))
			|| Url.Authority == TEXT("[::1]")
			|| Url.Authority.StartsWith(TEXT("[::1]:"));
		return Url.Scheme == TEXT("https")
			|| (Url.Scheme == TEXT("http") && bLoopback);
	}
}

namespace Haiven
{
	FHandoffCore::~FHandoffCore()
	{
		Cancel();
		ClearCredential();
	}

	FString FHandoffExchange::ToFormBody() const
	{
		return TEXT("code=") + Encode(Code)
			+ TEXT("&code_verifier=") + Encode(CodeVerifier)
			+ TEXT("&integration_id=") + Encode(IntegrationId)
			+ TEXT("&redirect_uri=") + Encode(RedirectUrl);
	}

	bool FHandoffCore::Begin(
		const FHandoffConfig& Config,
		TConstArrayView<uint8> StateEntropy,
		TConstArrayView<uint8> VerifierEntropy,
		const FDateTime& Now,
		FString& OutAuthorizationUrl,
		FString& OutState,
		FString& OutError)
	{
		FParsedUrl Entry;
		FParsedUrl Exchange;
		FParsedUrl Redirect;
		if (!ParseAbsoluteUrl(Config.EntryUrl, Entry) || !IsSecureWebUrl(Entry))
		{
			OutError = TEXT("The handoff entry URL must use HTTPS (loopback HTTP is allowed for development).");
			return false;
		}
		if (!ParseAbsoluteUrl(Config.ExchangeUrl, Exchange) || !IsSecureWebUrl(Exchange))
		{
			OutError = TEXT("The handoff exchange URL must use HTTPS (loopback HTTP is allowed for development).");
			return false;
		}
		if (!ParseAbsoluteUrl(Config.RedirectUrl, Redirect) || Config.IntegrationId.IsEmpty())
		{
			OutError = TEXT("An integration id and absolute fragment-free redirect URL are required.");
			return false;
		}
		if (Config.TransactionLifetime <= FTimespan::Zero() || StateEntropy.Num() != 32 || VerifierEntropy.Num() != 32)
		{
			OutError = TEXT("The transaction lifetime and 256-bit state/verifier entropy are required.");
			return false;
		}

		Cancel();
		ActiveConfig = Config;
		ActiveState = Base64Url(StateEntropy);
		ActiveVerifier = Base64Url(VerifierEntropy);
		CreatedAt = Now;
		OutState = ActiveState;
		OutAuthorizationUrl = BuildAuthorizationUrl(Config, ActiveState, PkceChallenge(ActiveVerifier));
		return true;
	}

	bool FHandoffCore::ConsumeCallback(
		const FString& CallbackUrl,
		const FDateTime& Now,
		FHandoffExchange& OutExchange,
		FString& OutError)
	{
		if (ActiveState.IsEmpty())
		{
			OutError = TEXT("The handoff state is unknown, expired, cancelled, or already consumed.");
			return false;
		}

		FParsedUrl Expected;
		FParsedUrl Callback;
		if (!ParseAbsoluteUrl(ActiveConfig.RedirectUrl, Expected)
			|| !ParseAbsoluteUrl(CallbackUrl, Callback)
			|| Expected.Scheme != Callback.Scheme
			|| Expected.Authority != Callback.Authority
			|| Expected.Path != Callback.Path)
		{
			OutError = TEXT("The handoff callback does not match the configured redirect URL.");
			return false;
		}
		for (const TPair<FString, FString>& Pair : Expected.Query)
		{
			const FString* Value = Callback.Query.Find(Pair.Key);
			if (Value == nullptr || *Value != Pair.Value)
			{
				OutError = TEXT("The handoff callback does not preserve the configured redirect query.");
				return false;
			}
		}

		const FString* State = Callback.Query.Find(TEXT("state"));
		if (State == nullptr || *State != ActiveState)
		{
			OutError = TEXT("The handoff callback state is missing or does not match.");
			return false;
		}

		const FString VerifierToConsume = MoveTemp(ActiveVerifier);
		SecureReset(ActiveState);
		SecureReset(ActiveVerifier);

		if (const FString* Error = Callback.Query.Find(TEXT("error")); Error != nullptr && !Error->IsEmpty())
		{
			OutError = TEXT("The handoff callback failed: ") + *Error;
			return false;
		}
		const FString* Code = Callback.Query.Find(TEXT("code"));
		if (Code == nullptr || Code->IsEmpty())
		{
			OutError = TEXT("The handoff callback has no code.");
			return false;
		}
		if (Now - CreatedAt > ActiveConfig.TransactionLifetime)
		{
			OutError = TEXT("The handoff transaction has expired.");
			return false;
		}

		OutExchange.ExchangeUrl = ActiveConfig.ExchangeUrl;
		OutExchange.Code = *Code;
		OutExchange.CodeVerifier = VerifierToConsume;
		OutExchange.IntegrationId = ActiveConfig.IntegrationId;
		OutExchange.RedirectUrl = ActiveConfig.RedirectUrl;
		return true;
	}

	void FHandoffCore::Cancel()
	{
		SecureReset(ActiveState);
		SecureReset(ActiveVerifier);
	}

	bool FHandoffCore::HasPendingTransaction() const
	{
		return !ActiveState.IsEmpty();
	}

	bool FHandoffCore::StoreExchangeResponse(
		const FString& Json,
		const FDateTime& Now,
		FHandoffCredential& OutCredential,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			OutError = TEXT("The handoff exchange returned invalid JSON.");
			return false;
		}
		if (Object->HasField(TEXT("refresh_token")))
		{
			OutError = TEXT("The handoff exchange returned a forbidden refresh credential.");
			return false;
		}
		FString AccessToken;
		FString TokenType;
		double ExpiresIn = -1.0;
		if (!Object->TryGetStringField(TEXT("access_token"), AccessToken)
			|| !Object->TryGetStringField(TEXT("token_type"), TokenType)
			|| !Object->TryGetNumberField(TEXT("expires_in"), ExpiresIn)
			|| AccessToken.IsEmpty() || TokenType.IsEmpty() || ExpiresIn < 0.0)
		{
			OutError = TEXT("The handoff exchange returned an invalid credential.");
			return false;
		}

		ClearCredential();
		OutCredential = {AccessToken, TokenType, Now + FTimespan::FromSeconds(ExpiresIn)};
		Credential = OutCredential;
		return true;
	}

	bool FHandoffCore::GetAccessToken(const FDateTime& Now, FString& OutAccessToken) const
	{
		if (!Credential.IsSet() || Credential->AccessToken.IsEmpty() || Now >= Credential->ExpiresAt) return false;
		OutAccessToken = Credential->AccessToken;
		return true;
	}

	void FHandoffCore::ClearCredential()
	{
		if (Credential.IsSet())
		{
			SecureReset(Credential->AccessToken);
			SecureReset(Credential->TokenType);
		}
		Credential.Reset();
	}

	FString FHandoffCore::PkceChallenge(const FString& Verifier)
	{
		FTCHARToUTF8 Utf8(*Verifier);
		uint8 Signature[SHA256_DIGEST_LENGTH];
		if (SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Signature) == nullptr) return FString();
		return Base64Url(MakeArrayView(Signature, UE_ARRAY_COUNT(Signature)));
	}
}
