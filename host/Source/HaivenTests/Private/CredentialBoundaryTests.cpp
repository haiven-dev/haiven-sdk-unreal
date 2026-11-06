#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "HaivenFrames.h"
#include "HaivenSubsystem.h"
#include "AuthExpiredTestSink.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenCredentialBoundary,
	"Haiven.Credentials.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace HaivenCredentialBoundaryDetail
{
	FHaivenHttpResult RewardReply(const FString& ClaimId)
	{
		FHaivenHttpResult Result;
		Result.bSucceeded = true;
		Result.StatusCode = 201;
		Result.Body = FString::Printf(
			TEXT(R"({"claimId":"%s","claimedAt":"2026-10-14T12:00:00Z","confirmation":"client_reported","integrationId":"game-1","rewardRef":"hat"})"),
			*ClaimId);
		return Result;
	}
}

void UHaivenAuthExpiredTestSink::HandleAuthExpired()
{
	++Calls;
	if (!Target) return;
	if (bReplaceAccount) Target->SetAccessToken(Token, ExpiresAtUtc);
	else Target->RefreshAccessToken(Token, ExpiresAtUtc);
}

bool FHaivenCredentialBoundary::RunTest(const FString& Parameters)
{
	using namespace HaivenCredentialBoundaryDetail;
	UHaivenSubsystem* Subsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	Subsystem->ResetChat();
	int32 ClaimIds = 0;
	Subsystem->Rewards = MakeUnique<FHaivenRewards>([&ClaimIds]
	{
		return "claim-" + std::to_string(++ClaimIds);
	});

	const FDateTime Expiry = FDateTime::UtcNow() + FTimespan::FromHours(1);
	Subsystem->SetAccessToken(TEXT("player-a"), Expiry);

	FHaivenOutgoingChatMessage ChatMessage;
	FString Error;
	TestTrue(TEXT("player A can queue chat before realtime is ready"),
		Subsystem->SendChatMessage(TEXT("ch-1"), TEXT("queued-a"), false, FString(), ChatMessage, Error));
	TestEqual(TEXT("player A queued chat is retained before the boundary"), Subsystem->GetChatOutbox().Num(), 1);
	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
	TestTrue(TEXT("a token replacement drops player A queued chat"), Subsystem->GetChatOutbox().IsEmpty());

	Subsystem->SetAccessToken(TEXT("player-a"), Expiry);
	std::vector<Haiven::FClientFrame> Written;
	int32 RequestIds = 0;
	Subsystem->RealtimeSession = MakeUnique<Haiven::FSessionCore>(
		[&Written](const Haiven::FClientFrame& Frame) { Written.push_back(Frame); },
		[] { return std::string("player-a"); },
		[&RequestIds] { return "a-" + std::to_string(++RequestIds); });
	Subsystem->RealtimeSession->OnReply = [Subsystem](const Haiven::FServerFrame& Frame, const std::string& Json)
	{
		if (Subsystem->Chat) Subsystem->Chat->HandleReply(Frame, Json);
	};
	Subsystem->RealtimeSession->OnClosed = [Subsystem](int, const std::string&)
	{
		if (Subsystem->Chat) Subsystem->Chat->HandleClosed();
	};
	Subsystem->Chat->Attach(*Subsystem->RealtimeSession);
	Subsystem->RealtimeSession->Opened();
	Haiven::FServerFrame Auth;
	const std::string AuthJson = R"({"type":"auth.success","requestId":"","success":true})";
	TestTrue(TEXT("the test realtime session authenticates"), Haiven::ParseServerFrame(AuthJson, Auth));
	Subsystem->RealtimeSession->Receive(Auth, AuthJson);
	TestTrue(TEXT("player A can start an in-flight chat send"),
		Subsystem->SendChatMessage(TEXT("ch-1"), TEXT("in-flight-a"), false, FString(), ChatMessage, Error));
	const auto Send = std::find_if(Written.begin(), Written.end(), [](const Haiven::FClientFrame& Frame)
	{
		return Frame.Type == "chat.send";
	});
	TestTrue(TEXT("player A chat reached the authenticated session"), Send != Written.end());
	if (Send == Written.end()) return false;
	const std::string PlayerARequestId = Send->RequestId;

	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
	TestTrue(TEXT("a token replacement drops player A in-flight chat state"), Subsystem->GetChatOutbox().IsEmpty());
	TestTrue(TEXT("player B can create independent chat state"),
		Subsystem->SendChatMessage(TEXT("ch-1"), TEXT("queued-b"), false, FString(), ChatMessage, Error));
	const FString PlayerBMessageId = ChatMessage.ClientMessageId;
	const std::string LateJson = R"({"type":"chat.send","requestId":")" + PlayerARequestId
		+ R"(","success":true,"payload":{"message":{"channelId":"ch-1","id":"message-a"},"moderationState":"allowed"}})";
	Haiven::FServerFrame LateFrame;
	TestTrue(TEXT("the stale player A chat reply parses"), Haiven::ParseServerFrame(LateJson, LateFrame));
	Subsystem->Chat->HandleReply(LateFrame, LateJson);
	const TArray<FHaivenOutgoingChatMessage> PlayerBOutbox = Subsystem->GetChatOutbox();
	TestTrue(TEXT("a stale player A reply cannot settle player B chat"), PlayerBOutbox.Num() == 1
		&& PlayerBOutbox[0].ClientMessageId == PlayerBMessageId
		&& !PlayerBOutbox[0].bHasMessageId);

	Subsystem->SetAccessToken(TEXT("player-a"), Expiry);
	FHaivenRestRequest RewardRequest;
	TestTrue(TEXT("player A can start a reward claim"),
		Subsystem->Rewards->ClaimRequest(TEXT("game-1"), TEXT("hat"), RewardRequest, Error));
	const uint64 PlayerAGeneration = Subsystem->Rewards->GetSessionGeneration();
	FString PlayerAClaimId;
	Subsystem->Rewards->GetPendingClaimId(TEXT("game-1"), TEXT("hat"), PlayerAClaimId);
	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
	FString Pending;
	TestFalse(TEXT("a token replacement drops player A pending reward id"),
		Subsystem->Rewards->GetPendingClaimId(TEXT("game-1"), TEXT("hat"), Pending));
	TestTrue(TEXT("player B can start an independent reward claim"),
		Subsystem->Rewards->ClaimRequest(TEXT("game-1"), TEXT("hat"), RewardRequest, Error));
	FString PlayerBClaimId;
	Subsystem->Rewards->GetPendingClaimId(TEXT("game-1"), TEXT("hat"), PlayerBClaimId);
	const TOptional<FHaivenRewardClaimOutcome> StaleReward = Subsystem->Rewards->HandleClaimReply(
		PlayerAGeneration, TEXT("game-1"), TEXT("hat"), PlayerAClaimId,
		RewardReply(PlayerAClaimId));
	TestFalse(TEXT("a stale player A reward completion is ignored"), StaleReward.IsSet());
	TestTrue(TEXT("player B reward state survives the stale completion"), PlayerBClaimId != PlayerAClaimId
		&& Subsystem->Rewards->GetPendingClaimId(TEXT("game-1"), TEXT("hat"), Pending)
		&& Pending == PlayerBClaimId);

	FHaivenClientOptions HttpOptions;
	HttpOptions.BaseUrl = TEXT("http://127.0.0.1:1");
	Subsystem->Configure(HttpOptions);
	TSharedRef<TPromise<FHaivenHttpResult>, ESPMode::ThreadSafe> CancelPromise =
		MakeShared<TPromise<FHaivenHttpResult>, ESPMode::ThreadSafe>();
	TFuture<FHaivenHttpResult> CancelFuture = CancelPromise->GetFuture();
	bool bCancellationSawPlayerC = false;
	const uint64 PlayerAHttpGeneration = Subsystem->CredentialGeneration;
	const FGuid AuthenticatedHandle = Subsystem->Request(
		TEXT("GET"), TEXT("/credential-boundary"), FString(), FHaivenHttpComplete::CreateLambda(
		[Subsystem, CancelPromise, &bCancellationSawPlayerC](const FHaivenHttpResult& Result)
		{
			FString Token;
			bCancellationSawPlayerC = Subsystem->GetAccessToken(Token) && Token == TEXT("player-c");
			CancelPromise->SetValue(Result);
		}));
	TestTrue(TEXT("an authenticated HTTP operation is pending under player A"),
		Subsystem->HttpRequests.Contains(AuthenticatedHandle)
		&& Subsystem->HttpRequests[AuthenticatedHandle].bAuthenticated
		&& Subsystem->HttpRequests[AuthenticatedHandle].CredentialGeneration == PlayerAHttpGeneration);
	Subsystem->SetAccessToken(TEXT("player-c"), Expiry);
	TestTrue(TEXT("token replacement advances the HTTP credential generation"),
		Subsystem->CredentialGeneration > PlayerAHttpGeneration);
	TestFalse(TEXT("token replacement removes the player A HTTP operation"),
		Subsystem->HttpRequests.Contains(AuthenticatedHandle));
	TestTrue(TEXT("the detached delegate-backed promise completes exactly once"), CancelFuture.IsReady());
	TestEqual(TEXT("credential replacement completes old HTTP work as cancelled"),
		CancelFuture.Get().Error, FString(TEXT("cancelled")));
	TestTrue(TEXT("the cancellation callback observes player C, never player A"), bCancellationSawPlayerC);

	bool bStaleGenerationCompleted = false;
	const FGuid StaleHandle = FGuid::NewGuid();
	UHaivenSubsystem::FPendingHttp StalePending;
	StalePending.bAuthenticated = true;
	StalePending.CredentialGeneration = PlayerAHttpGeneration;
	StalePending.Completion = FHaivenHttpComplete::CreateLambda(
		[&bStaleGenerationCompleted](const FHaivenHttpResult&) { bStaleGenerationCompleted = true; });
	Subsystem->HttpRequests.Add(StaleHandle, MoveTemp(StalePending));
	FHaivenHttpResult LateHttpResult;
	LateHttpResult.bSucceeded = true;
	LateHttpResult.StatusCode = 200;
	Subsystem->CompleteHttpRequest(StaleHandle, PlayerAHttpGeneration, LateHttpResult);
	TestFalse(TEXT("a stale authenticated HTTP generation cannot complete under player C"), bStaleGenerationCompleted);

	UHaivenAuthExpiredTestSink* UnauthorizedSink = NewObject<UHaivenAuthExpiredTestSink>(Subsystem->GetOuter());
	Subsystem->OnAuthExpired.AddDynamic(UnauthorizedSink, &UHaivenAuthExpiredTestSink::HandleAuthExpired);
	const FGuid UnauthorizedHandle = FGuid::NewGuid();
	UHaivenSubsystem::FPendingHttp UnauthorizedPending;
	UnauthorizedPending.bAuthenticated = true;
	UnauthorizedPending.CredentialGeneration = Subsystem->CredentialGeneration;
	Subsystem->HttpRequests.Add(UnauthorizedHandle, MoveTemp(UnauthorizedPending));
	FHaivenHttpResult UnauthorizedResult;
	UnauthorizedResult.StatusCode = 401;
	Subsystem->CompleteHttpRequest(UnauthorizedHandle, Subsystem->CredentialGeneration, UnauthorizedResult);
	TestEqual(TEXT("an authenticated REST 401 asks the host to re-handoff"), UnauthorizedSink->Calls, 1);
	Subsystem->OnAuthExpired.RemoveAll(UnauthorizedSink);

	bool bPublicCancelled = false;
	const FGuid PublicHandle = FGuid::NewGuid();
	UHaivenSubsystem::FPendingHttp PublicPending;
	PublicPending.bAuthenticated = false;
	PublicPending.CredentialGeneration = Subsystem->CredentialGeneration;
	PublicPending.Completion = FHaivenHttpComplete::CreateLambda(
		[&bPublicCancelled](const FHaivenHttpResult& Result)
		{
			bPublicCancelled = Result.Error == TEXT("cancelled");
		});
	Subsystem->HttpRequests.Add(PublicHandle, MoveTemp(PublicPending));
	TestTrue(TEXT("a public HTTP operation is pending without account ownership"),
		Subsystem->HttpRequests.Contains(PublicHandle) && !Subsystem->HttpRequests[PublicHandle].bAuthenticated);
	Subsystem->SetAccessToken(TEXT("player-d"), Expiry);
	TestTrue(TEXT("public HTTP survives an unrelated credential boundary"), Subsystem->HttpRequests.Contains(PublicHandle));
	Subsystem->CancelHttpRequest(PublicHandle);
	TestTrue(TEXT("the surviving public request remains explicitly cancellable"), bPublicCancelled);

	UHaivenSubsystem* RejectionSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	RejectionSubsystem->SetAccessToken(TEXT("retained-on-403"), Expiry);
	const FGuid ForbiddenHandle = FGuid::NewGuid();
	UHaivenSubsystem::FPendingHttp ForbiddenPending;
	ForbiddenPending.bAuthenticated = true;
	ForbiddenPending.CredentialGeneration = RejectionSubsystem->CredentialGeneration;
	RejectionSubsystem->HttpRequests.Add(ForbiddenHandle, MoveTemp(ForbiddenPending));
	FHaivenHttpResult Forbidden;
	Forbidden.StatusCode = 403;
	Forbidden.Error = TEXT("forbidden");
	RejectionSubsystem->CompleteHttpRequest(
		ForbiddenHandle, RejectionSubsystem->CredentialGeneration, Forbidden);
	FString RetainedToken;
	TestTrue(TEXT("HTTP 403 is an authority refusal and retains the bearer"),
		RejectionSubsystem->GetAccessToken(RetainedToken) && RetainedToken == TEXT("retained-on-403"));

	const FGuid RejectionUnauthorizedHandle = FGuid::NewGuid();
	UHaivenSubsystem::FPendingHttp RejectionUnauthorizedPending;
	RejectionUnauthorizedPending.bAuthenticated = true;
	RejectionUnauthorizedPending.CredentialGeneration = RejectionSubsystem->CredentialGeneration;
	RejectionSubsystem->HttpRequests.Add(RejectionUnauthorizedHandle, MoveTemp(RejectionUnauthorizedPending));
	FHaivenHttpResult Unauthorized;
	Unauthorized.StatusCode = 401;
	Unauthorized.Error = TEXT("unauthorized");
	RejectionSubsystem->CompleteHttpRequest(
		RejectionUnauthorizedHandle, RejectionSubsystem->CredentialGeneration, Unauthorized);
	TestFalse(TEXT("HTTP 401 clears the rejected bearer"), RejectionSubsystem->GetAccessToken(RetainedToken));

	Subsystem->Logout();

	FHaivenClientOptions RealtimeOptions;
	RealtimeOptions.RealtimeUrl = TEXT("wss://127.0.0.1:1");
	UHaivenSubsystem* CloseCallbackSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	CloseCallbackSubsystem->Configure(RealtimeOptions);
	CloseCallbackSubsystem->SetAccessToken(TEXT("player-a"), Expiry);
	CloseCallbackSubsystem->RealtimeSession = MakeUnique<Haiven::FSessionCore>(
		[](const Haiven::FClientFrame&) {}, [] { return std::string("player-a"); }, [] { return std::string("old-1"); });
	Haiven::FSessionCore* OldCloseSession = CloseCallbackSubsystem->RealtimeSession.Get();
	const uint64 OldCloseGeneration = CloseCallbackSubsystem->RealtimeGeneration;
	OldCloseSession->OnClosed = [CloseCallbackSubsystem](int, const std::string&)
	{
		CloseCallbackSubsystem->ConnectRealtime();
	};
	OldCloseSession->Opened();
	CloseCallbackSubsystem->SetAccessToken(TEXT("player-b"), Expiry);
	FString CloseCallbackToken;
	TestTrue(TEXT("the replacement token is active before the old close callback reconnects"),
		CloseCallbackSubsystem->GetAccessToken(CloseCallbackToken) && CloseCallbackToken == TEXT("player-b"));
	TestNull(TEXT("the old close callback cannot reconnect inside credential teardown"),
		CloseCallbackSubsystem->RealtimeSession.Get());
	CloseCallbackSubsystem->RunDeferredRealtimeConnect();
	TestTrue(TEXT("a reconnect created by the old close callback survives outer teardown"),
		CloseCallbackSubsystem->RealtimeSession && CloseCallbackSubsystem->RealtimeGeneration > OldCloseGeneration);
	CloseCallbackSubsystem->DisconnectRealtime();

	UHaivenSubsystem* CompletionSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	CompletionSubsystem->Configure(RealtimeOptions);
	CompletionSubsystem->SetAccessToken(TEXT("player-a"), Expiry);
	CompletionSubsystem->RealtimeSession = MakeUnique<Haiven::FSessionCore>(
		[](const Haiven::FClientFrame&) {}, [] { return std::string("player-a"); }, [] { return std::string("old-2"); });
	Haiven::FSessionCore* OldCompletionSession = CompletionSubsystem->RealtimeSession.Get();
	const uint64 OldCompletionGeneration = CompletionSubsystem->RealtimeGeneration;
	CompletionSubsystem->RealtimeSession->OnClosed = [CompletionSubsystem](int, const std::string& Reason)
	{
		CompletionSubsystem->CompleteConnect(false, UTF8_TO_TCHAR(Reason.c_str()));
	};
	CompletionSubsystem->ConnectCompletions.Add(FHaivenConnectComplete::CreateLambda(
		[CompletionSubsystem, Expiry](bool, const FString&)
		{
			CompletionSubsystem->SetAccessToken(TEXT("player-b"), Expiry);
			CompletionSubsystem->ConnectRealtime();
		}));
	OldCompletionSession->Opened();
	CompletionSubsystem->DisconnectRealtime();
	FString CompletionToken;
	TestTrue(TEXT("a token change from the old completion callback becomes active"),
		CompletionSubsystem->GetAccessToken(CompletionToken) && CompletionToken == TEXT("player-b"));
	TestNull(TEXT("the completion callback reconnect waits until outer teardown unwinds"),
		CompletionSubsystem->RealtimeSession.Get());
	CompletionSubsystem->RunDeferredRealtimeConnect();
	TestTrue(TEXT("a reconnect created by the old completion callback survives outer teardown"),
		CompletionSubsystem->RealtimeSession && CompletionSubsystem->RealtimeGeneration > OldCompletionGeneration);
	CompletionSubsystem->DisconnectRealtime();

	// OnAuthExpired runs inside FSessionCore::HandleExpired, which reads the token
	// source and writes auth after the broadcast returns. Both setters are driven
	// through the subsystem's real session wiring and dynamic delegate.
	const std::string AuthenticatedJson = R"({"type":"auth.success","requestId":"","success":true})";
	const std::string ExpiredJson = R"({"type":"auth.expired","requestId":"","success":false})";
	Haiven::FServerFrame Authenticated;
	Haiven::FServerFrame Expired;
	TestTrue(TEXT("the auth-success frame parses"), Haiven::ParseServerFrame(AuthenticatedJson, Authenticated));
	TestTrue(TEXT("the auth-expired frame parses"), Haiven::ParseServerFrame(ExpiredJson, Expired));
	auto BindExpiringSession = [Expiry](
		UHaivenSubsystem* Target,
		std::vector<Haiven::FClientFrame>& Writes,
		int32& RequestIds,
		bool bReplaceAccount,
		const FString& NextToken)
	{
		Target->ResetChat();
		Target->RealtimeSession = MakeUnique<Haiven::FSessionCore>(
			[&Writes](const Haiven::FClientFrame& Frame) { Writes.push_back(Frame); },
			[Target]
			{
				FString Token;
				return Target->GetAccessToken(Token) ? std::string(TCHAR_TO_UTF8(*Token)) : std::string();
			},
			[&RequestIds] { return "expiry-" + std::to_string(++RequestIds); });
		++Target->RealtimeGeneration;
		Target->BindRealtimeSession();
		UHaivenAuthExpiredTestSink* Sink = NewObject<UHaivenAuthExpiredTestSink>(Target->GetOuter());
		Sink->Target = Target;
		Sink->bReplaceAccount = bReplaceAccount;
		Sink->Token = NextToken;
		Sink->ExpiresAtUtc = Expiry;
		Target->OnAuthExpired.AddDynamic(Sink, &UHaivenAuthExpiredTestSink::HandleAuthExpired);
		return Sink;
	};

	UHaivenSubsystem* RefreshSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	RefreshSubsystem->SetAccessToken(TEXT("expired-bearer"), Expiry);
	std::vector<Haiven::FClientFrame> RefreshWrites;
	int32 RefreshRequestIds = 0;
	UHaivenAuthExpiredTestSink* RefreshSink = BindExpiringSession(
		RefreshSubsystem, RefreshWrites, RefreshRequestIds, false, TEXT("fresh-bearer"));
	Haiven::FSessionCore* const LiveRefreshSession = RefreshSubsystem->RealtimeSession.Get();
	LiveRefreshSession->Opened();
	LiveRefreshSession->Receive(Authenticated, AuthenticatedJson);
	TestEqual(TEXT("the refresh session is ready before expiry"),
		RefreshSubsystem->GetRealtimeState(), EHaivenRealtimeState::Ready);
	LiveRefreshSession->Receive(Expired, ExpiredJson);
	TestEqual(TEXT("OnAuthExpired reached the game listener once"), RefreshSink->Calls, 1);
	TestTrue(TEXT("same-account refresh keeps the live realtime session"),
		RefreshSubsystem->RealtimeSession.Get() == LiveRefreshSession);
	TestTrue(TEXT("same-account refresh retires no session"), RefreshSubsystem->RetiredRealtimeSessions.IsEmpty());
	TestTrue(TEXT("same-account refresh does not close the socket"),
		LiveRefreshSession->GetState() == Haiven::ESessionState::Expired
		&& RefreshSubsystem->GetRealtimeState() == EHaivenRealtimeState::Authenticating);
	TestTrue(TEXT("same-socket re-auth uses the refreshed bearer"), RefreshWrites.size() == 2
		&& RefreshWrites.back().Type == Haiven::AuthType
		&& RefreshWrites.back().PayloadJson == std::optional<std::string>("fresh-bearer"));
	LiveRefreshSession->Receive(Authenticated, AuthenticatedJson);
	TestEqual(TEXT("the refreshed session becomes ready again on the same socket"),
		RefreshSubsystem->GetRealtimeState(), EHaivenRealtimeState::Ready);
	TestEqual(TEXT("same-account refresh records no realtime drop"), RefreshSubsystem->RealtimeDropCount, 0);
	RefreshSubsystem->DisconnectRealtime();
	RefreshSubsystem->ReleaseRetiredRealtimeSessions();

	UHaivenSubsystem* ReplaceSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	ReplaceSubsystem->SetAccessToken(TEXT("player-a"), Expiry);
	std::vector<Haiven::FClientFrame> ReplaceWrites;
	int32 ReplaceRequestIds = 0;
	UHaivenAuthExpiredTestSink* ReplaceSink = BindExpiringSession(
		ReplaceSubsystem, ReplaceWrites, ReplaceRequestIds, true, TEXT("player-b"));
	Haiven::FSessionCore* const ExpiringSession = ReplaceSubsystem->RealtimeSession.Get();
	ExpiringSession->Opened();
	ExpiringSession->Receive(Authenticated, AuthenticatedJson);
	ExpiringSession->Receive(Expired, ExpiredJson);
	TestEqual(TEXT("the replacing listener ran once"), ReplaceSink->Calls, 1);
	FString ReplacementToken;
	TestTrue(TEXT("account replacement from OnAuthExpired installs the new bearer"),
		ReplaceSubsystem->GetAccessToken(ReplacementToken) && ReplacementToken == TEXT("player-b"));
	TestNull(TEXT("account replacement from OnAuthExpired detaches the expired session"),
		ReplaceSubsystem->RealtimeSession.Get());
	TestTrue(TEXT("the expired session outlives the callback that replaced the account"),
		ReplaceSubsystem->RetiredRealtimeSessions.Num() == 1
		&& ReplaceSubsystem->RetiredRealtimeSessions[0].Get() == ExpiringSession
		&& ExpiringSession->GetState() == Haiven::ESessionState::Closed);
	TestEqual(TEXT("account replacement closes realtime"),
		ReplaceSubsystem->GetRealtimeState(), EHaivenRealtimeState::Disconnected);
	ReplaceSubsystem->ReleaseRetiredRealtimeSessions();
	TestTrue(TEXT("the retired session is released on a later turn"),
		ReplaceSubsystem->RetiredRealtimeSessions.IsEmpty());
	return true;
}
#endif
