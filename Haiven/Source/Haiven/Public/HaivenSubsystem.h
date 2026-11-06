#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "HaivenChat.h"
#include "HaivenClientTypes.h"
#include "HaivenHandoff.h"
#include "HaivenIdentity.h"
#include "HaivenRewards.h"
#include "HaivenHealth.h"
#include "HaivenIdentityCore.h"
#include "HaivenSessionCore.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HaivenSubsystem.generated.h"

class FHaivenAnalytics;
class FHaivenLoopbackReceiver;

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenGameLoginOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString EntryUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString ExchangeUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString IntegrationId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString RedirectUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString InstructionsId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString ScreenHint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth", meta = (ClampMin = "1.0"))
	float TransactionLifetimeSeconds = 900.0f;
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenGameCredential
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	FString AccessToken;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	FString TokenType;

	/** FDateTime::MaxValue() for a durable credential, which has no fixed expiry. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	FDateTime ExpiresAtUtc;

	/** False for a durable credential, which lasts until it is revoked (ADR 0041). */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	bool bHasExpiry = true;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenGameLoginCompleted, const FHaivenGameCredential&, Credential);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenGameLoginFailed, const FString&, Error);

/** Game-instance owner for Haiven's one-use browser login handoff. */
UCLASS(BlueprintType)
class HAIVEN_API UHaivenSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Haiven")
	void Configure(const FHaivenClientOptions& InOptions);

	/**
	 * Replaces the signed-in account with a host-owned bearer. Every account-owned
	 * transport and queue is reset and realtime is closed. This is also safe from
	 * an OnAuthExpired listener when the account changed: the expired socket is
	 * closed instead of re-authenticated, and it is released after the listener
	 * returns.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void SetAccessToken(const FString& AccessToken, FDateTime ExpiresAtUtc);

	/**
	 * Updates the bearer for the current account without tearing down its live
	 * realtime/chat/identity state. Use this from OnAuthExpired after refreshing
	 * the same account; use SetAccessToken when the account may have changed.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void RefreshAccessToken(const FString& AccessToken, FDateTime ExpiresAtUtc);

	/** Creates PKCE/state, stores the verifier transiently, and opens the system browser. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	bool BeginGameLogin(const FHaivenGameLoginOptions& Options, FString& AuthorizationUrl, FString& Error);

	/**
	 * Desktop sign-in with no deep-link glue: listens on the loopback redirect in
	 * Options.RedirectUrl (`http://127.0.0.1:<port>/<path>` or
	 * `http://localhost:<port>/<path>`, registered exactly on the integration),
	 * opens the system browser like BeginGameLogin, and hands the browser's
	 * redirect to the same callback validation. Only this device's own browser is
	 * answered, and a request that does not belong to the transaction is refused
	 * without ending it. The route closes when the transaction ends: completed,
	 * failed, cancelled, logged out, or unanswered for TransactionLifetimeSeconds.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	bool BeginLoopbackGameLogin(const FHaivenGameLoginOptions& Options, FString& AuthorizationUrl, FString& Error);

	/**
	 * Callback adapter for a platform deep-link handler. Validation happens before
	 * the form exchange; the verifier is consumed once even when the exchange fails.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void HandleGameLoginCallback(const FString& CallbackUrl);

	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void CancelGameLogin();

	/** True from Begin Game Login until its callback has been exchanged or the login ended. */
	UFUNCTION(BlueprintPure, Category = "Haiven|Auth")
	bool IsGameLoginInProgress() const;

	/**
	 * Replaces how a game login opens its authorization URL, for an in-game
	 * browser, a platform overlay, or a test. The launcher returns false with an
	 * error to abandon the login. An unset launcher uses FPlatformProcess::LaunchURL.
	 */
	void SetLoginUrlLauncher(TFunction<bool(const FString& Url, FString& Error)> Launcher);

	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void Logout();

	/**
	 * Keeps the game-login credential in Store across restarts, so
	 * ResumeGameLogin can reuse it. Without one the credential lasts only until
	 * the game exits, which suits development, not a shipped game. Logout and a
	 * refused credential clear it there too; shutting down does not.
	 */
	void SetCredentialStore(TSharedPtr<Haiven::IHandoffCredentialStore> Store);

	/**
	 * Signs in with the credential the store saved, without opening the browser.
	 * Call it at start, before Begin Game Login; false means nothing valid is
	 * saved, and an expired one is cleared. Success raises OnGameLoginCompleted
	 * like a finished login.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	bool ResumeGameLogin();

	UFUNCTION(BlueprintPure, Category = "Haiven|Auth")
	bool GetAccessToken(FString& AccessToken) const;

	/** Starts one authenticated REST request and returns its cancellation handle. */
	FGuid Request(
		const FString& Method,
		const FString& Path,
		const FString& BodyJson,
		FHaivenHttpComplete Completion);
	TFuture<FHaivenHttpResult> RequestAsync(
		const FString& Method,
		const FString& Path,
		const FString& BodyJson = FString());
	/** Public request without a bearer, used by the pre-sign-in MOTD read. */
	FGuid RequestPublic(
		const FString& Method,
		const FString& Path,
		const FString& BodyJson,
		FHaivenHttpComplete Completion);

	UFUNCTION(BlueprintCallable, Category = "Haiven|HTTP")
	void CancelHttpRequest(FGuid RequestHandle);

	/** Connects and completes only after first-frame authentication succeeds. */
	void ConnectRealtime(FHaivenConnectComplete Completion = FHaivenConnectComplete());
	TFuture<bool> ConnectRealtimeAsync();

	/** Closes realtime deliberately and cancels a scheduled reconnect. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Realtime")
	void DisconnectRealtime();

	/** True while an unexpected close is waiting out its backoff before reconnecting. */
	UFUNCTION(BlueprintPure, Category = "Haiven|Realtime")
	bool IsRealtimeReconnectScheduled() const { return RealtimeReconnectTicker.IsValid(); }

	FString SendRealtimeRequest(
		const FString& Type,
		const FString& PayloadJson,
		FHaivenRealtimeComplete Completion);
	TFuture<FHaivenRealtimeReply> SendRealtimeRequestAsync(
		const FString& Type,
		const FString& PayloadJson = FString());

	UFUNCTION(BlueprintCallable, Category = "Haiven|Realtime")
	void CancelRealtimeRequest(const FString& RequestId);

	UFUNCTION(BlueprintPure, Category = "Haiven|Realtime")
	EHaivenRealtimeState GetRealtimeState() const { return RealtimeState; }

	/**
	 * Sends a chat message through the subsystem's chat (FHaivenChat), or keeps
	 * it to send when the game next connects. A send cut off by a closed socket
	 * is resent under the same client message id on the next connect, which the
	 * server deduplicates. Fails, sending nothing, only for an empty channel id.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Chat", meta = (AdvancedDisplay = "bHasParentMessageId,ParentMessageId"))
	bool SendChatMessage(
		const FString& ChannelId,
		const FString& Content,
		bool bHasParentMessageId,
		const FString& ParentMessageId,
		FHaivenOutgoingChatMessage& Message,
		FString& Error);

	/** Messages this player sent, oldest first, until Forget Chat Message. */
	UFUNCTION(BlueprintPure, Category = "Haiven|Chat")
	TArray<FHaivenOutgoingChatMessage> GetChatOutbox() const;

	/** Stops tracking a sent message, normally one in a final state. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Chat")
	void ForgetChatMessage(const FString& ClientMessageId);

	/**
	 * A snapshot of connection, auth, request, and chat-outbox health, plus the
	 * most recent failure and close. Safe to poll every frame; it never exposes
	 * the access token.
	 */
	UFUNCTION(BlueprintPure, Category = "Haiven|Health")
	FHaivenHealth GetHealth() const;

	/** The chat game C++ can drive directly; null before Initialize. */
	FHaivenChat* GetChat() const { return Chat.Get(); }

	/**
	 * The claim id the next Claim Reward of this reward will reuse, when an
	 * earlier claim of it has not had a final answer (it failed, or was
	 * cancelled). False when the next claim mints a new one.
	 */
	UFUNCTION(BlueprintPure, Category = "Haiven|Rewards")
	bool GetPendingRewardClaimId(const FString& IntegrationId, const FString& RewardRef, FString& ClaimId) const;

	/**
	 * Records a reward the player asked for before linking. Keeps the claim id
	 * and does not post it, so nothing is granted yet.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Rewards")
	bool SaveRewardClaim(const FString& IntegrationId, const FString& RewardRef, FString& ClaimId, FString& Error);

	/** The claim id Save Reward Claim stored for this reward, if it is still waiting to be submitted. */
	UFUNCTION(BlueprintPure, Category = "Haiven|Rewards")
	bool GetSavedRewardClaimId(const FString& IntegrationId, const FString& RewardRef, FString& ClaimId) const;

	/** The reward claims Claim Reward drives, for game C++; null before Initialize. */
	FHaivenRewards* GetRewards() const { return Rewards.Get(); }

	/** Injects the browser-cookie context required by the identity-link POST. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Identity")
	void SetIdentityBrowserContextAdapter(UHaivenBrowserContextAdapter* Adapter);

	/** Starts same-browser POST + redirect linking; requires a ready realtime session. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Identity")
	bool BeginIdentityLink(const FString& Provider, FString& Error);

	UFUNCTION(BlueprintCallable, Category = "Haiven|Identity")
	void CancelIdentityLink();

	/** Public pre-sign-in MOTD read for an integration id. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|MOTD")
	void LoadMotd(const FString& IntegrationId);

	/** Suppresses the current message id for this subsystem session. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|MOTD")
	bool DismissMotd();

	/** The action boundary accepted by the MOTD parser: absolute HTTPS only. */
	UFUNCTION(BlueprintPure, Category = "Haiven|MOTD")
	static bool IsValidMotdActionUrl(const FString& Url);

	UFUNCTION(BlueprintCallable, Category = "Haiven|Identity")
	void RefreshSignInMethods();

	UFUNCTION(BlueprintCallable, Category = "Haiven|Identity")
	void UnlinkIdentity(const FString& Provider);

	UFUNCTION(BlueprintPure, Category = "Haiven|MOTD")
	bool GetCurrentMotd(FHaivenMotd& Message) const;

	UFUNCTION(BlueprintPure, Category = "Haiven|Identity")
	bool GetCurrentSignInMethods(FHaivenSignInMethods& Methods) const;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Auth")
	FHaivenGameLoginCompleted OnGameLoginCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Auth")
	FHaivenGameLoginFailed OnGameLoginFailed;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeStateChanged OnRealtimeStateChanged;

	/** Every inbound frame, including auth frames consumed by the state machine. */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeFrameReceived OnRealtimeFrame;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeFrameReceived OnRealtimeEvent;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeClosed OnRealtimeClosed;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeError OnRealtimeError;

	/** Fires whenever a message this player sent changes state. */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Chat")
	FHaivenOutgoingChatChanged OnOutgoingChatChanged;

	/** Fires whenever a Claim Reward is recorded (Claimed or AlreadyRecorded). Grant loot only from this. */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Rewards")
	FHaivenRewardClaimed OnRewardClaimed;

	/**
	 * Fires for every saved claim submitted after linking, including a refusal.
	 * A refusal is not loot. Claimed and AlreadyRecorded are also reported on OnRewardClaimed.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Rewards")
	FHaivenRewardClaimed OnRewardClaimSettled;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|MOTD")
	FHaivenMotdChanged OnMotdChanged;

	/** Fires when a public MOTD read fails; diagnostics remain on OnIdentityDiagnostic. */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|MOTD")
	FHaivenMotdLoadFailed OnMotdLoadFailed;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Identity")
	FHaivenSignInMethodsChanged OnSignInMethodsChanged;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Identity")
	FHaivenIdentityLinked OnIdentityLinked;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Identity")
	FHaivenIdentityLinkFailed OnIdentityLinkFailed;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Identity")
	FHaivenIdentityDiagnostic OnIdentityDiagnostic;

	/**
	 * Fires before same-socket re-auth reads the token owner again. Call
	 * RefreshAccessToken here to continue the same account on the live socket;
	 * SetAccessToken here is an account replacement and closes it.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Auth")
	FHaivenAuthExpired OnAuthExpired;

private:
	friend class FHaivenDiagnosticsHealth;
	friend class FHaivenAnalytics;
	friend class FHaivenAnalyticsBridge;
	friend class FHaivenChatStarterReconnectTest;
	friend class FHaivenCredentialBoundary;
	friend class FHaivenIdentityBridge;
	friend class FHaivenLoopbackLogin;
	friend class FHaivenRealtimeReconnect;

	/** Logs a failure as a Warning and keeps it as the health snapshot's last error. */
	void RecordFailure(EHaivenHealthArea Area, const FString& Error);
	/** Keeps bounded, content-free frame metadata for GetHealth and the editor tab. */
	void RecordRealtimeFrame(EHaivenRealtimeFrameDirection Direction, const FString& Type);
	void FinishExchange(bool bTransportSucceeded, int32 StatusCode, const FString& Body, const FString& TransportError);
	/** Makes a new or resumed game-login credential the active account. */
	void ActivateGameCredential(const Haiven::FHandoffCredential& Credential);
	bool BeginGameLoginTransaction(const FHaivenGameLoginOptions& Options, FString& AuthorizationUrl, FString& Error);
	/** False, broadcasting nothing, when the callback is not this transaction's and it is still pending. */
	bool ConsumeLoopbackCallback(const FString& CallbackUrl);
	bool ConsumeGameLoginCallback(const FString& CallbackUrl, bool bQuietWhilePending);
	void StopLoopbackReceiver();
	void BroadcastFailureOnGameThread(const FString& Error);
	void ShutdownNetworking();
	void SetRealtimeState(EHaivenRealtimeState NewState);
	void HandleSocketConnected(uint64 Generation);
	void HandleSocketMessage(uint64 Generation, const FString& Json);
	void HandleSocketError(uint64 Generation, const FString& Error);
	void HandleSocketClosed(uint64 Generation, int32 Code, const FString& Reason);
	/** Installs the subsystem's state, frame, expiry, and close handlers on RealtimeSession. */
	void BindRealtimeSession();
	/** Keeps a torn-down session alive until its own callback stack has unwound. */
	void RetireRealtimeSession(TUniquePtr<Haiven::FSessionCore> Session);
	void ReleaseRetiredRealtimeSessions();
	void WriteClientFrame(const Haiven::FClientFrame& Frame);
	void HandleRealtimeReply(const Haiven::FServerFrame& Frame, const std::string& Json);
	void CompleteConnect(bool bSucceeded, const FString& Error);
	void FailRealtimeRequests(const FString& Error);
	void HandleIdentityRealtimeEvent(const Haiven::FServerFrame& Frame, const std::string& Json);
	void BroadcastIdentityDiagnosticOnGameThread(const FString& Error);
	void ResetIdentity();
	void RegisterAnalytics(const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe>& Instance);
	TArray<FHaivenHttpComplete> InvalidateAuthenticatedHttpRequests();
	void InvalidateCredentialBoundary();
	/** Clears a server-rejected bearer and every account-owned operation. */
	void ClearRejectedCredential();
	void CompleteHttpRequest(FGuid Handle, uint64 RequestCredentialGeneration, const FHaivenHttpResult& Result);
	void RunDeferredRealtimeConnect();
	/** Drops a scheduled reconnect; a connect or disconnect the game asked for also restarts the backoff. */
	void CancelRealtimeReconnect();
	/** After an unexpected close: reconnects later unless the game acted, the option is off, or the account cannot continue. */
	void ScheduleRealtimeReconnect(uint64 Intent, int32 Code, const FString& Reason);
	void RunRealtimeReconnect();
	/** Capped exponential backoff with equal jitter: Jitter in [0, 1] picks within the upper half of the step. */
	static float RealtimeReconnectDelaySeconds(int32 Attempt, float InitialSeconds, float MaxSeconds, float Jitter);
	FGuid RequestInternal(bool bAuthenticated, const FString& Method, const FString& Path, const FString& BodyJson, FHaivenHttpComplete Completion);
	void ResetChat();
	/** Posts saved claim intents on the user claim route. No-op when none are saved. */
	void SubmitSavedRewardClaims();
	void FinishSavedRewardClaim(uint64 Session, const FString& IntegrationId, const FString& RewardRef, const FString& ClaimId, const FHaivenHttpResult& Result);
	bool ValidatePayload(const FString& PayloadJson, FString& Error) const;
	FString ResolveHttpUrl(const FString& Path) const;

	struct FPendingHttp
	{
		TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> Request;
		FHaivenHttpComplete Completion;
		uint64 CredentialGeneration = 0;
		bool bAuthenticated = false;
	};

	TUniquePtr<Haiven::FHandoffCore> Handoff;
	TFunction<bool(const FString& Url, FString& Error)> LoginUrlLauncher;
	// The loopback route for the current BeginLoopbackGameLogin, and its deadline.
	TSharedPtr<FHaivenLoopbackReceiver> LoopbackReceiver;
	FTSTicker::FDelegateHandle LoopbackDeadline;
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
	FHaivenClientOptions ClientOptions;
	FString HostAccessToken;
	FDateTime HostAccessTokenExpiry;
	uint64 CredentialGeneration = 0;
	TMap<FGuid, FPendingHttp> HttpRequests;
	TSharedPtr<class IWebSocket> RealtimeSocket;
	TUniquePtr<Haiven::FSessionCore> RealtimeSession;
	// Closed sessions whose callbacks may still be on the stack; released next tick.
	TArray<TUniquePtr<Haiven::FSessionCore>> RetiredRealtimeSessions;
	// Outlives sessions: each ConnectRealtime attaches its new session.
	TUniquePtr<FHaivenChat> Chat;
	// Pending claim ids are the signed-in player's; Logout clears them.
	TUniquePtr<FHaivenRewards> Rewards;
	TUniquePtr<Haiven::FIdentityCore> Identity;
	// Batches are bearer-bound. A credential replacement shuts down every live
	// subsystem adapter so queued/retry work cannot move to the next account.
	TArray<TWeakPtr<FHaivenAnalytics, ESPMode::ThreadSafe>> AnalyticsInstances;
	UPROPERTY(Transient) TObjectPtr<UHaivenBrowserContextAdapter> IdentityBrowserAdapter;
	TMap<FString, FHaivenRealtimeComplete> RealtimeRequests;
	TArray<FHaivenConnectComplete> ConnectCompletions;
	TArray<FHaivenConnectComplete> DeferredConnectCompletions;
	EHaivenRealtimeState RealtimeState = EHaivenRealtimeState::Disconnected;
	uint64 RealtimeGeneration = 0;
	bool bRealtimeTeardown = false;
	bool bRealtimeConnectDeferred = false;
	bool bRealtimeConnectScheduled = false;
	bool bRealtimeRetirementScheduled = false;
	// Moves whenever the game connects or disconnects, so a close whose callbacks
	// did either is not reconnected behind its back.
	uint64 RealtimeConnectIntent = 0;
	FTSTicker::FDelegateHandle RealtimeReconnectTicker;
	// Consecutive reconnects since the last Ready or the game's own connect.
	int32 RealtimeReconnectAttempt = 0;
	float RealtimeReconnectDelay = 0.0f;
	bool bRealtimeReconnecting = false;
	// The server refused this session's credential, so a reconnect would be refused too.
	bool bRealtimeAuthRefused = false;
	// Health counters; see FHaivenHealth.
	int32 RealtimeReadyCount = 0;
	int32 RealtimeDropCount = 0;
	bool bHasLastClose = false;
	int32 LastCloseCode = 0;
	FString LastCloseReason;
	EHaivenHealthArea LastErrorArea = EHaivenHealthArea::None;
	FString LastError;
	FDateTime LastErrorAtUtc;
	TArray<FHaivenRecentRealtimeFrame> RecentRealtimeFrames;
};
