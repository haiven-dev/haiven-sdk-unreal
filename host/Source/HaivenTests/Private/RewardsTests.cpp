#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenRewards.h"
#include "HaivenSubsystem.h"
#include "RewardsTestSink.h"
#include "Engine/GameInstance.h"

#include <string>

// The plugin's reward claims (FHaivenRewards over the C++ SDK's FRewardClaims).
// The core's claim-id and refusal rules are sdks/cpp/test/rewards_test.cpp's;
// this proves the engine half: the claim request as a Blueprint REST request, a
// reply read from an FHaivenHttpResult into the Blueprint outcome (the recorded
// claim through the generated converter), a retry that reuses the claim id, and
// the reflected Claim Reward node and subsystem surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenRewardsBridge,
	"Haiven.Rewards.Bridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace HaivenRewardsTestsDetail
{
	FHaivenHttpResult Reply(int32 Status, const FString& Body)
	{
		FHaivenHttpResult Result;
		Result.StatusCode = Status;
		Result.bSucceeded = Status >= 200 && Status < 300;
		Result.Body = Body;
		return Result;
	}
}

void UHaivenRewardsTestSink::LogoutAndStartNextPlayer(const FHaivenRewardClaimOutcome&)
{
	++SubsystemClaimedCount;
	if (!Target) return;
	Target->Logout();
	FHaivenRewards* Rewards = Target->GetRewards();
	FHaivenRestRequest Request;
	FString Error;
	bNextPlayerStarted = Rewards
		&& Rewards->ClaimRequest(TEXT("game-1"), TEXT("callback-scarf"), Request, Error)
		&& Rewards->GetPendingClaimId(TEXT("game-1"), TEXT("callback-scarf"), NextPlayerClaimId);
}

void UHaivenRewardsTestSink::RecordClaimed(const FHaivenRewardClaimOutcome&)
{
	++ActionClaimedCount;
}

void UHaivenRewardsTestSink::RecordFailed(const FHaivenRewardClaimOutcome& Outcome)
{
	++ActionFailedCount;
	FailedOutcome = Outcome;
}

bool FHaivenRewardsBridge::RunTest(const FString& Parameters)
{
	using namespace HaivenRewardsTestsDetail;
	int Ids = 0;
	FHaivenRewards Rewards([&Ids] { return "claim-" + std::to_string(++Ids); });
	const uint64 FirstSession = Rewards.GetSessionGeneration();

	FHaivenRestRequest Request;
	FString Error;
	TestFalse(TEXT("an empty integration id builds nothing"), Rewards.ClaimRequest(FString(), TEXT("hat"), Request, Error));
	FHaivenRewards ValidationRewards([] { return "validation-claim"; });
	TestTrue(TEXT("a 128-code-point non-ASCII reward ref builds"),
		ValidationRewards.ClaimRequest(TEXT("game-1"), FString::ChrN(128, TEXT('\x0436')), Request, Error));
	TestFalse(TEXT("a 129-code-point non-ASCII reward ref builds nothing"),
		ValidationRewards.ClaimRequest(TEXT("game-1"), FString::ChrN(129, TEXT('\x0436')), Request, Error));
	TestFalse(TEXT("and says why"), Error.IsEmpty());

	TestTrue(TEXT("a claim builds"), Rewards.ClaimRequest(TEXT("game-1"), TEXT("hat"), Request, Error));
	TestTrue(TEXT("as POST /users/me/rewards/claims under a minted claim id"), Request.Method == TEXT("POST")
		&& Request.Path == TEXT("/users/me/rewards/claims") && Request.Body.Contains(TEXT("\"claimId\":\"claim-1\"")));

	FHaivenHttpResult Lost;
	Lost.Error = TEXT("connection reset");
	const TOptional<FHaivenRewardClaimOutcome> Failed = Rewards.HandleClaimReply(
		FirstSession, TEXT("game-1"), TEXT("hat"), TEXT("claim-1"), Lost);
	TestTrue(TEXT("a lost reply is a retryable failure with the transport's reason"), Failed.IsSet()
		&& Failed.GetValue().Result == EHaivenRewardClaimResult::Failed
		&& Failed.GetValue().bRetryable && Failed.GetValue().Error == TEXT("connection reset"));
	FString Pending;
	TestTrue(TEXT("its claim id stays pending"), Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("hat"), Pending) && Pending == TEXT("claim-1"));
	Rewards.ClaimRequest(TEXT("game-1"), TEXT("hat"), Request, Error);
	TestTrue(TEXT("and the retry reuses it"), Request.Body.Contains(TEXT("\"claimId\":\"claim-1\"")));

	const TOptional<FHaivenRewardClaimOutcome> Recorded = Rewards.HandleClaimReply(FirstSession, TEXT("game-1"), TEXT("hat"), TEXT("claim-1"), Reply(200,
		TEXT(R"({"claimId":"claim-1","claimedAt":"2026-10-10T00:00:00Z","confirmation":"client_reported","integrationId":"game-1","rewardRef":"hat"})")));
	TestTrue(TEXT("200 is the already-recorded claim, decoded into its Blueprint mirror"), Recorded.IsSet()
		&& Recorded.GetValue().Result == EHaivenRewardClaimResult::AlreadyRecorded
		&& Recorded.GetValue().bHasClaim && Recorded.GetValue().Claim.bHasClaimId && Recorded.GetValue().Claim.ClaimId == TEXT("claim-1")
		&& Recorded.GetValue().Confirmation == EHaivenRewardConfirmation::ClientReported);
	TestFalse(TEXT("a final answer releases the claim id"), Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("hat"), Pending));

	Rewards.ClaimRequest(TEXT("game-1"), TEXT("cape"), Request, Error);
	Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("cape"), Pending);
	const TOptional<FHaivenRewardClaimOutcome> Refused = Rewards.HandleClaimReply(FirstSession, TEXT("game-1"), TEXT("cape"), Pending,
		Reply(409, TEXT(R"({"code":"reward_already_claimed","message":"already","status":409})")));
	TestTrue(TEXT("a refusal keeps its finite code"), Refused.IsSet()
		&& Refused.GetValue().Result == EHaivenRewardClaimResult::AlreadyClaimed
		&& Refused.GetValue().ErrorCode == TEXT("reward_already_claimed")
		&& !Refused.GetValue().bRetryable && !Refused.GetValue().bHasClaim);

	Rewards.ClaimRequest(TEXT("game-1"), TEXT("boots"), Request, Error);
	FString PlayerAClaimId;
	Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("boots"), PlayerAClaimId);
	Rewards.Clear();
	TestFalse(TEXT("Clear forgets every pending claim id"), Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("boots"), Pending));
	Rewards.ClaimRequest(TEXT("game-1"), TEXT("boots"), Request, Error);
	FString PlayerBClaimId;
	Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("boots"), PlayerBClaimId);
	const FString StaleBody = FString::Printf(
		TEXT(R"({"claimId":"%s","claimedAt":"2026-10-10T00:00:00Z","confirmation":"client_reported","integrationId":"game-1","rewardRef":"boots"})"),
		*PlayerAClaimId);
	const TOptional<FHaivenRewardClaimOutcome> Stale = Rewards.HandleClaimReply(
		FirstSession, TEXT("game-1"), TEXT("boots"), PlayerAClaimId, Reply(201, StaleBody));
	TestFalse(TEXT("a completion from before logout is ignored for the next player"), Stale.IsSet());
	TestTrue(TEXT("the next player's pending claim id survives that stale completion"),
		PlayerBClaimId != PlayerAClaimId
		&& Rewards.GetPendingClaimId(TEXT("game-1"), TEXT("boots"), Pending) && Pending == PlayerBClaimId);

	FHaivenRewards Default;
	TestTrue(TEXT("the default claim id source builds a claim"), Default.ClaimRequest(TEXT("game-1"), TEXT("hat"), Request, Error));
	TestTrue(TEXT("its claim id is a UUIDv7"), Default.GetPendingClaimId(TEXT("game-1"), TEXT("hat"), Pending)
		&& Pending.Len() == 36 && Pending[14] == TEXT('7'));

	UGameInstance* GameInstance = NewObject<UGameInstance>();
	GameInstance->Init();
	UHaivenSubsystem* PlayerSubsystem = GameInstance->GetSubsystem<UHaivenSubsystem>();
	TestNotNull(TEXT("a game instance initializes the Haiven subsystem"), PlayerSubsystem);
	if (PlayerSubsystem)
	{
		FHaivenRewards* PlayerRewards = PlayerSubsystem->GetRewards();
		TestNotNull(TEXT("the initialized subsystem owns reward state"), PlayerRewards);
		if (PlayerRewards)
		{
			FHaivenRestRequest CallbackRequest;
			TestTrue(TEXT("the callback test claim builds"), PlayerRewards->ClaimRequest(
				TEXT("game-1"), TEXT("callback-scarf"), CallbackRequest, Error));
			FString CallbackClaimId;
			PlayerRewards->GetPendingClaimId(TEXT("game-1"), TEXT("callback-scarf"), CallbackClaimId);

			UHaivenRewardsTestSink* Sink = NewObject<UHaivenRewardsTestSink>(GameInstance);
			Sink->SetTarget(PlayerSubsystem);
			PlayerSubsystem->OnRewardClaimed.AddDynamic(Sink, &UHaivenRewardsTestSink::LogoutAndStartNextPlayer);
			UHaivenClaimRewardAsyncAction* Action = NewObject<UHaivenClaimRewardAsyncAction>(GameInstance);
			Action->Owner = PlayerSubsystem;
			Action->Integration = TEXT("game-1");
			Action->Reward = TEXT("callback-scarf");
			Action->ClaimId = CallbackClaimId;
			Action->SessionGeneration = PlayerRewards->GetSessionGeneration();
			Action->Claimed.AddDynamic(Sink, &UHaivenRewardsTestSink::RecordClaimed);
			Action->Failed.AddDynamic(Sink, &UHaivenRewardsTestSink::RecordFailed);

			Action->Finish(Reply(201, FString::Printf(
				TEXT(R"({"claimId":"%s","claimedAt":"2026-10-10T00:00:00Z","confirmation":"client_reported","integrationId":"game-1","rewardRef":"callback-scarf"})"),
				*CallbackClaimId)));

			TestEqual(TEXT("the subsystem delegate receives the recorded claim"), Sink->SubsystemClaimedCount, 1);
			TestEqual(TEXT("logout from that delegate suppresses the old action's Claimed pin"), Sink->ActionClaimedCount, 0);
			TestEqual(TEXT("the invalidated action still settles exactly once through Failed"), Sink->ActionFailedCount, 1);
			TestTrue(TEXT("the invalidation outcome is final and carries no old claim"),
				!Sink->FailedOutcome.bRetryable && !Sink->FailedOutcome.bHasClaim);
			TestTrue(TEXT("the next player's pending claim survives the old action completion"),
				Sink->bNextPlayerStarted && Sink->NextPlayerClaimId != CallbackClaimId
					&& PlayerRewards->GetPendingClaimId(TEXT("game-1"), TEXT("callback-scarf"), Pending)
					&& Pending == Sink->NextPlayerClaimId);

			// Logout while a claim is in flight, before its reply arrives: the old
			// node must still settle through exactly one pin rather than vanish.
			FHaivenRestRequest InFlightRequest;
			TestTrue(TEXT("the in-flight test claim builds"), PlayerRewards->ClaimRequest(
				TEXT("game-1"), TEXT("inflight-gloves"), InFlightRequest, Error));
			FString InFlightClaimId;
			PlayerRewards->GetPendingClaimId(TEXT("game-1"), TEXT("inflight-gloves"), InFlightClaimId);
			UHaivenRewardsTestSink* InFlightSink = NewObject<UHaivenRewardsTestSink>(GameInstance);
			UHaivenClaimRewardAsyncAction* InFlight = NewObject<UHaivenClaimRewardAsyncAction>(GameInstance);
			InFlight->Owner = PlayerSubsystem;
			InFlight->Integration = TEXT("game-1");
			InFlight->Reward = TEXT("inflight-gloves");
			InFlight->ClaimId = InFlightClaimId;
			InFlight->SessionGeneration = PlayerRewards->GetSessionGeneration();
			InFlight->Claimed.AddDynamic(InFlightSink, &UHaivenRewardsTestSink::RecordClaimed);
			InFlight->Failed.AddDynamic(InFlightSink, &UHaivenRewardsTestSink::RecordFailed);

			PlayerSubsystem->Logout();
			FHaivenRestRequest NextRequest;
			FString NextClaimId;
			const bool bNextStarted = PlayerRewards->ClaimRequest(TEXT("game-1"), TEXT("inflight-gloves"), NextRequest, Error)
				&& PlayerRewards->GetPendingClaimId(TEXT("game-1"), TEXT("inflight-gloves"), NextClaimId);
			const int32 SubsystemClaimsBefore = Sink->SubsystemClaimedCount;

			InFlight->Finish(Reply(201, FString::Printf(
				TEXT(R"({"claimId":"%s","claimedAt":"2026-10-10T00:00:00Z","confirmation":"client_reported","integrationId":"game-1","rewardRef":"inflight-gloves"})"),
				*InFlightClaimId)));

			TestEqual(TEXT("a reply after logout never reaches the subsystem delegate"),
				Sink->SubsystemClaimedCount, SubsystemClaimsBefore);
			TestEqual(TEXT("a reply after logout never fires the old node's Claimed pin"), InFlightSink->ActionClaimedCount, 0);
			TestEqual(TEXT("the old node settles exactly once through Failed after logout"), InFlightSink->ActionFailedCount, 1);
			TestTrue(TEXT("that settlement is final and carries no old claim"),
				!InFlightSink->FailedOutcome.bRetryable && !InFlightSink->FailedOutcome.bHasClaim);
			TestTrue(TEXT("the next player's pending claim survives the in-flight completion"),
				bNextStarted && NextClaimId != InFlightClaimId
					&& PlayerRewards->GetPendingClaimId(TEXT("game-1"), TEXT("inflight-gloves"), Pending)
					&& Pending == NextClaimId);
		}
	}
	GameInstance->Shutdown();

	UClass* Subsystem = UHaivenSubsystem::StaticClass();
	const UFunction* PendingFunction = Subsystem->FindFunctionByName(TEXT("GetPendingRewardClaimId"));
	TestTrue(TEXT("the pending claim id is readable from Blueprint"), PendingFunction != nullptr && PendingFunction->HasAnyFunctionFlags(FUNC_BlueprintPure));
	TestNotNull(TEXT("the claimed delegate is assignable"), Subsystem->FindPropertyByName(TEXT("OnRewardClaimed")));
	const UFunction* Node = UHaivenClaimRewardAsyncAction::StaticClass()->FindFunctionByName(TEXT("ClaimReward"));
	TestTrue(TEXT("the claim node is Blueprint-callable"), Node != nullptr && Node->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	for (const TCHAR* Pin : {TEXT("Claimed"), TEXT("Refused"), TEXT("Failed")})
	{
		TestNotNull(FString::Printf(TEXT("the claim node has a %s pin"), Pin), UHaivenClaimRewardAsyncAction::StaticClass()->FindPropertyByName(Pin));
	}
	return true;
}
#endif
