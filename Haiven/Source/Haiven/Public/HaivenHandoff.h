#pragma once

#include "CoreMinimal.h"
#include "HaivenHandoffCore.h"

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
		/** Meaningful only when bHasExpiry. */
		FDateTime ExpiresAt;
		/** False for a durable credential, which lasts until it is revoked (ADR 0041). */
		bool bHasExpiry = true;
	};

	/**
	 * Where a game keeps its credential across restarts (a save slot, a platform
	 * keychain), so FHandoffCore::Resume can reuse it instead of opening the
	 * browser. Without one the credential is memory-only, which suits development
	 * and tests, not a shipped game.
	 */
	class IHandoffCredentialStore
	{
	public:
		virtual ~IHandoffCredentialStore() = default;
		/** Writes the saved credential and returns true, or returns false when none is saved. */
		virtual bool Load(FHandoffCredential& OutCredential) = 0;
		virtual void Store(const FHandoffCredential& Credential) = 0;
		virtual void Clear() = 0;
	};

	/**
	 * The handoff state machine for Unreal callers: `Haiven::Handoff::FCore` from
	 * the C++ SDK (`HaivenHandoffCore.h`), which owns every rule, behind engine
	 * strings and `FDateTime`. The verifier is memory-only and is removed before
	 * an exchange can begin, making callback redemption atomic.
	 */
	class HAIVEN_API FHandoffCore
	{
	public:

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
		/** Erases the credential in memory and in the credential store. */
		void ClearCredential();

		/** Saves every stored credential in Store as well; null returns to memory-only. */
		void SetCredentialStore(TSharedPtr<IHandoffCredentialStore> Store);

		/**
		 * Reuses the credential the store saved. False means nothing valid is
		 * saved, and the caller begins a handoff; an expired one is cleared.
		 */
		bool Resume(const FDateTime& Now, FHandoffCredential& OutCredential);

		bool Restart(
			Handoff::ECredentialEndReason Reason,
			const FHandoffConfig& Config,
			TConstArrayView<uint8> StateEntropy,
			TConstArrayView<uint8> VerifierEntropy,
			const FDateTime& Now,
			FString& OutAuthorizationUrl,
			FString& OutState,
			FString& OutError);

		static FString PkceChallenge(const FString& Verifier);

	private:
		/** Converts the core's credential to engine types for the game's store. */
		class FStoreBridge final : public Handoff::ICredentialStore
		{
		public:
			TSharedPtr<IHandoffCredentialStore> Target;
			virtual bool Load(Handoff::FCredential& OutCredential) override;
			virtual void Store(const Handoff::FCredential& Credential) override;
			virtual void Clear() override;
		};

		// Declared before Core, so it outlives the core that points at it.
		FStoreBridge StoreBridge;
		Handoff::FCore Core;
	};
}
