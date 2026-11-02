#pragma once

#include "CoreMinimal.h"

namespace Haiven
{
	struct FHandoffConfig
	{
		FString EntryUrl;
		FString ExchangeUrl;
		FString IntegrationId;
		FString RedirectUrl;
		FString InstructionsId;
		FString ScreenHint;
		FTimespan TransactionLifetime = FTimespan::FromMinutes(15);
	};

	struct HAIVEN_API FHandoffExchange
	{
		FString ExchangeUrl;
		FString Code;
		FString CodeVerifier;
		FString IntegrationId;
		FString RedirectUrl;

		FString ToFormBody() const;
	};

	struct FHandoffCredential
	{
		FString AccessToken;
		FString TokenType;
		FDateTime ExpiresAt;
	};

	/**
	 * Target-independent handoff state machine. The verifier is memory-only and
	 * is removed before an exchange can begin, making callback redemption atomic.
	 */
	class HAIVEN_API FHandoffCore
	{
	public:
		~FHandoffCore();

		bool Begin(
			const FHandoffConfig& Config,
			TConstArrayView<uint8> StateEntropy,
			TConstArrayView<uint8> VerifierEntropy,
			const FDateTime& Now,
			FString& OutAuthorizationUrl,
			FString& OutState,
			FString& OutError);

		bool ConsumeCallback(
			const FString& CallbackUrl,
			const FDateTime& Now,
			FHandoffExchange& OutExchange,
			FString& OutError);

		void Cancel();
		bool HasPendingTransaction() const;

		bool StoreExchangeResponse(
			const FString& Json,
			const FDateTime& Now,
			FHandoffCredential& OutCredential,
			FString& OutError);

		bool GetAccessToken(const FDateTime& Now, FString& OutAccessToken) const;
		void ClearCredential();

		static FString PkceChallenge(const FString& Verifier);

	private:
		FHandoffConfig ActiveConfig;
		FString ActiveState;
		FString ActiveVerifier;
		FDateTime CreatedAt;
		TOptional<FHandoffCredential> Credential;
	};
}
