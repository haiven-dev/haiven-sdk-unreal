#pragma once

#include "CoreMinimal.h"
#include "Blueprint/HaivenBlueprintRestTypes.h"
#include "HaivenClientTypes.h"
#include "HaivenRewardsCore.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "HaivenRewards.generated.h"

class UHaivenSubsystem;

/** How one reward claim ended (Haiven::ERewardClaimResult). */
UENUM(BlueprintType)
enum class EHaivenRewardClaimResult : uint8
{
	/** The claim was recorded by this request. */
	Claimed,
	/** The same claim id was already recorded: this is that claim. */
	AlreadyRecorded,
	/** The account holds no active Haiven subscription (`reward_not_eligible`). */
	NotEligible,
	/** This player already claimed this reward in this period (`reward_already_claimed`). */
	AlreadyClaimed,
	/** The claim id was used for a different claim (`reward_claim_conflict`). */
	ClaimConflict,
	/** The period is being refunded, so it takes no new claims (`refund_in_progress`). */
	RefundInProgress,
	/** The claim to confirm does not exist, or belongs to another integration. */
	NotFound,
	/** Anything else: transport failure, server error, bad request, unreadable reply. */
	Failed,
};

/** Who vouched for a claim (Haiven::ERewardConfirmation). */
UENUM(BlueprintType)
enum class EHaivenRewardConfirmation : uint8
{
	/** Only the player's client vouched for it. */
	ClientReported,
	/** The game's server confirmed it, or granted it. */
	ServerConfirmed,
	/** Absent, or a value this build does not know: never read as confirmed. */
	Unrecognized,
};

/** A saved claim ready to post on POST /users/me/rewards/claims. Not a recorded claim. */
struct HAIVEN_API FHaivenSavedClaimSubmission
{
	FString IntegrationId;
	FString RewardRef;
	FString ClaimId;
	FHaivenRestRequest Request;
};

/** One finished reward claim (Haiven::FRewardClaimOutcome). */
USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenRewardClaimOutcome
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	EHaivenRewardClaimResult Result = EHaivenRewardClaimResult::Failed;

	/** The HTTP status; 0 when the request never reached the service. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	int32 StatusCode = 0;

	/** The recorded claim, when Result is Claimed or AlreadyRecorded. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	bool bHasClaim = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	FHaivenRestRewardClaimResponse Claim;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	EHaivenRewardConfirmation Confirmation = EHaivenRewardConfirmation::Unrecognized;

	/** The refusal's finite error code, verbatim; empty when none. Branch on this, not on Error. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	FString ErrorCode;

	/** A human-readable reason for Failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	FString Error;

	/** Claiming the same reward again may succeed, and reuses this claim's id, so it cannot claim twice. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Rewards")
	bool bRetryable = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenRewardClaimed, const FHaivenRewardClaimOutcome&, Outcome);

/**
 * The engine-side half of the plugin's subscriber-reward claims: owns the C++ SDK's Haiven::FRewardClaims, which keeps the claim
 * id of every unanswered claim so a retry is the same claim to the service,
 * and presents its outcomes in Blueprint types. UHaivenSubsystem owns one; it
 * is public so a game (and the automation tests) can drive one over its own
 * HTTP stack.
 *
 * Not thread-safe; use it on one thread (the subsystem uses the game thread).
 */
class HAIVEN_API FHaivenRewards
{
public:
	/** Claim ids are UUIDv7s from the UTC clock and a fresh GUID's bytes. */
	FHaivenRewards();
	explicit FHaivenRewards(Haiven::FRewardClaims::FClaimIdSource NewClaimId);

	FHaivenRewards(const FHaivenRewards&) = delete;
	FHaivenRewards& operator=(const FHaivenRewards&) = delete;

	/**
	 * The claim request for one reward, reusing the claim id of an unanswered
	 * earlier attempt. Fails, building nothing, for an empty integration id or
	 * a reward ref that is not 1 to 128 characters.
	 */
	bool ClaimRequest(const FString& IntegrationId, const FString& RewardRef, FHaivenRestRequest& OutRequest, FString& OutError);

	/** The reply to a ClaimRequest; stale replies from an earlier signed-in player are ignored. */
	TOptional<FHaivenRewardClaimOutcome> HandleClaimReply(
		uint64 RequestSessionGeneration,
		const FString& IntegrationId,
		const FString& RewardRef,
		const FString& RequestClaimId,
		const FHaivenHttpResult& Result);

	/** Identifies the signed-in player's pending-claim state. */
	uint64 GetSessionGeneration() const { return Core.GetSessionGeneration(); }

	/** The claim id the next claim of this reward will carry, if one is pending. */
	bool GetPendingClaimId(const FString& IntegrationId, const FString& RewardRef, FString& OutClaimId) const;

	/**
	 * Records a claim before the player links. Reuses a saved or pending claim
	 * id and does not build a request. False when the integration id or reward
	 * ref is not acceptable. Not a recorded claim: do not grant loot.
	 */
	bool SaveClaim(const FString& IntegrationId, const FString& RewardRef, FString& OutClaimId, FString& OutError);

	/** The claim id that will be submitted for this reward after linking, if one was saved. */
	bool GetSavedClaimId(const FString& IntegrationId, const FString& RewardRef, FString& OutClaimId) const;

	/**
	 * Remembers ExportSaved in this file and reloads it. Empty when no file has
	 * been chosen. The file holds claim ids, not credentials.
	 */
	void UseSavedClaimsFile(const FString& Path);

	/** One user-claim request per saved intent, under the id SaveClaim kept. */
	void CollectSavedSubmissions(TArray<FHaivenSavedClaimSubmission>& Out);

	/** Forgets every pending claim id: the player signed out. Saved intents stay. */
	void Clear();

	static FHaivenRewardClaimOutcome ToBlueprint(const Haiven::FRewardClaimOutcome& Outcome);
	static EHaivenRewardClaimResult ToBlueprint(Haiven::ERewardClaimResult Result);
	static EHaivenRewardConfirmation ToBlueprint(Haiven::ERewardConfirmation Confirmation);

private:
	void PersistSavedIfChanged(const std::string& Before);

	Haiven::FRewardClaims Core;
	FString SavedClaimsFile;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenClaimRewardOutput, const FHaivenRewardClaimOutcome&, Outcome);

/**
 * Claims one subscriber reward for the signed-in player, as client_reported
 * (POST /users/me/rewards/claims). The claim id is minted and kept by the
 * subsystem: running this node again for the same reward after Failed reuses
 * it, so a retry never claims twice. The game's server can then upgrade the
 * claim to server_confirmed with its integration key.
 *
 * Exactly one output pin fires.
 */
UCLASS()
class HAIVEN_API UHaivenClaimRewardAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "Haiven|Rewards")
	static UHaivenClaimRewardAsyncAction* ClaimReward(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem,
		const FString& IntegrationId,
		const FString& RewardRef);

	virtual void Activate() override;

	/** Stops waiting. The claim may still be recorded; claiming again reuses its id. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Rewards")
	void Cancel();

	/** Recorded: Result is Claimed, or AlreadyRecorded for a retry the service had already taken. */
	UPROPERTY(BlueprintAssignable)
	FHaivenClaimRewardOutput Claimed;

	/** Refused with a finite code: NotEligible, AlreadyClaimed, ClaimConflict or RefundInProgress. */
	UPROPERTY(BlueprintAssignable)
	FHaivenClaimRewardOutput Refused;

	/** Not recorded for another reason; bRetryable says whether claiming again may succeed. */
	UPROPERTY(BlueprintAssignable)
	FHaivenClaimRewardOutput Failed;

private:
	friend class FHaivenRewardsBridge;

	void Finish(const FHaivenHttpResult& Result);
	void Fail(const FString& Error);

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
	FString Integration;
	FString Reward;
	FString ClaimId;
	FGuid Handle;
	uint64 SessionGeneration = 0;
};
