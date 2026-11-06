#pragma once

#include "Contracts/HaivenRestJson.h"
#include "HaivenRestApi.h"

#include "HaivenJson.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Haiven
{
	/** How one reward claim (or grant, or confirmation) ended. */
	enum class ERewardClaimResult
	{
		/** The claim was recorded by this request (HTTP 201). */
		Claimed,
		/** The same claim id was already recorded: this is that claim (HTTP 200). */
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

	/** Who vouched for a claim, from RewardClaimResponse.confirmation. */
	enum class ERewardConfirmation
	{
		/** Only the player's client vouched for it. */
		ClientReported,
		/** The game's server confirmed it (or granted it). */
		ServerConfirmed,
		/** Absent, or a value minted after this build: never read as confirmed. */
		Unrecognized,
	};

	/** One finished reward request: its result and, on success, the recorded claim. */
	struct FRewardClaimOutcome
	{
		ERewardClaimResult Result = ERewardClaimResult::Failed;
		/** The HTTP status; 0 for a transport failure. */
		int Status = 0;
		/** The recorded claim, when Result is Claimed or AlreadyRecorded. */
		std::optional<RestTypes::RewardClaimResponse> Claim;
		ERewardConfirmation Confirmation = ERewardConfirmation::Unrecognized;
		/** The finite error code of a refusal (ErrorResponse.code), verbatim; empty when none. */
		std::string ErrorCode;
		/** A human-readable reason for Failed. Never branch on it: branch on Result and ErrorCode. */
		std::string Error;
		/**
		 * Repeating the same request may succeed: a transport failure or a server
		 * error. A retry through FRewardClaims reuses the claim id, so it cannot
		 * claim twice.
		 */
		bool bRetryable = false;

		bool Succeeded() const { return Result == ERewardClaimResult::Claimed || Result == ERewardClaimResult::AlreadyRecorded; }
	};

	/**
	 * A UUIDv7 (RFC 9562) from a Unix time in milliseconds and ten random bytes:
	 * 48 bits of time, the version, 12 + 62 random bits, and the variant.
	 */
	inline std::string FormatUuidV7(std::uint64_t UnixMilliseconds, const std::array<std::uint8_t, 10>& Random)
	{
		std::array<std::uint8_t, 16> Bytes{};
		for (int Index = 0; Index < 6; ++Index)
			Bytes[Index] = static_cast<std::uint8_t>((UnixMilliseconds >> (8 * (5 - Index))) & 0xFF);
		Bytes[6] = static_cast<std::uint8_t>(0x70 | (Random[0] & 0x0F));
		Bytes[7] = Random[1];
		Bytes[8] = static_cast<std::uint8_t>(0x80 | (Random[2] & 0x3F));
		for (int Index = 0; Index < 7; ++Index) Bytes[9 + Index] = Random[3 + Index];
		static const char Hex[] = "0123456789abcdef";
		std::string Out;
		Out.reserve(36);
		for (int Index = 0; Index < 16; ++Index)
		{
			if (Index == 4 || Index == 6 || Index == 8 || Index == 10) Out += '-';
			Out += Hex[Bytes[Index] >> 4];
			Out += Hex[Bytes[Index] & 0x0F];
		}
		return Out;
	}

	/** Counts Unicode code points, treating each malformed UTF-8 byte as one replacement character. */
	inline std::size_t Utf8CodePointCount(const std::string& Value)
	{
		const auto IsContinuation = [](unsigned char Byte) { return Byte >= 0x80 && Byte <= 0xBF; };
		std::size_t Count = 0;
		for (std::size_t Index = 0; Index < Value.size(); ++Count)
		{
			const unsigned char First = static_cast<unsigned char>(Value[Index]);
			std::size_t Width = 1;
			if (First >= 0xC2 && First <= 0xDF && Index + 1 < Value.size()
				&& IsContinuation(static_cast<unsigned char>(Value[Index + 1])))
			{
				Width = 2;
			}
			else if (First >= 0xE0 && First <= 0xEF && Index + 2 < Value.size())
			{
				const unsigned char Second = static_cast<unsigned char>(Value[Index + 1]);
				const bool bValidSecond = (First == 0xE0 && Second >= 0xA0 && Second <= 0xBF)
					|| (First == 0xED && Second >= 0x80 && Second <= 0x9F)
					|| (((First >= 0xE1 && First <= 0xEC) || (First >= 0xEE && First <= 0xEF)) && IsContinuation(Second));
				if (bValidSecond && IsContinuation(static_cast<unsigned char>(Value[Index + 2]))) Width = 3;
			}
			else if (First >= 0xF0 && First <= 0xF4 && Index + 3 < Value.size())
			{
				const unsigned char Second = static_cast<unsigned char>(Value[Index + 1]);
				const bool bValidSecond = (First == 0xF0 && Second >= 0x90 && Second <= 0xBF)
					|| (First == 0xF4 && Second >= 0x80 && Second <= 0x8F)
					|| ((First >= 0xF1 && First <= 0xF3) && IsContinuation(Second));
				if (bValidSecond && IsContinuation(static_cast<unsigned char>(Value[Index + 2]))
					&& IsContinuation(static_cast<unsigned char>(Value[Index + 3]))) Width = 4;
			}
			Index += Width;
		}
		return Count;
	}

	/**
	 * One reward the player asked for before a Haiven account was linked.
	 * Local only: it is not a recorded claim, and it is not loot.
	 */
	struct FSavedRewardClaim
	{
		std::string ClaimId;
		std::string IntegrationId;
		std::string RewardRef;
	};

	/**
	 * Subscriber-reward claims, engine-free: the C++ counterpart of
	 * the .NET and JavaScript SDKs' reward clients, and the core the Unreal
	 * plugin's Claim Reward node drives.
	 *
	 * A player claims a reward with POST /users/me/rewards/claims under a claim
	 * id the client mints (a UUIDv7). The service records a claim once per
	 * player, game and period, and a repeat of the same claim id returns the
	 * recorded claim, so a retry is safe only when it reuses the id. This keeps
	 * the id of every claim that has not reached a final answer, keyed by
	 * integration and reward, and hands the same id to the next attempt; a final
	 * answer (recorded, or refused) releases it.
	 *
	 * Save records that id before linking and does not post it. The host keeps
	 * ExportSaved across launches. After the login handoff, SubmitSavedRequests
	 * posts each saved id on the same user claim route. Succeeded is the only
	 * signal that the server recorded the claim; Save never sets it, so loot
	 * waits for that reply. Clearing the signed-in player drops in-flight ids
	 * and leaves saved intents, so the next link retries the same claim id.
	 *
	 * Like the chat core it performs no I/O: build a request here, send it with
	 * the host's HTTP stack, and pass the status and body back. A game server
	 * holding an integration key builds its grant and confirm requests here too,
	 * and classifies their replies the same way.
	 */
	class FRewardClaims
	{
	public:
		using FClaimIdSource = std::function<std::string()>;

		explicit FRewardClaims(FClaimIdSource InNewClaimId)
			: NewClaimId(std::move(InNewClaimId))
		{
			if (!NewClaimId) throw std::invalid_argument("haiven: a claim id source is required");
		}

		/**
		 * The claim request for one reward, under the claim id of an earlier
		 * unanswered attempt at the same reward, or a fresh one.
		 */
		Rest::FRestRequest ClaimRequest(const std::string& IntegrationId, const std::string& RewardRef)
		{
			if (IntegrationId.empty()) throw std::invalid_argument("haiven: integrationId is required");
			if (RewardRef.empty() || Utf8CodePointCount(RewardRef) > 128) throw std::invalid_argument("haiven: rewardRef must be 1 to 128 characters");
			RestTypes::ClaimRewardRequest Body;
			Body.ClaimId = ClaimIdFor(IntegrationId, RewardRef);
			Body.IntegrationId = IntegrationId;
			Body.RewardRef = RewardRef;
			return Rest::PostUsersMeRewardsClaims(ContractsJson::ToJson(Body));
		}

		/**
		 * Records a claim intent before the player links. Mints a claim id once,
		 * or reuses the id already saved or pending for this reward. Does not
		 * build a request and does not report the claim as recorded.
		 */
		FSavedRewardClaim Save(const std::string& IntegrationId, const std::string& RewardRef)
		{
			if (IntegrationId.empty()) throw std::invalid_argument("haiven: integrationId is required");
			if (RewardRef.empty() || Utf8CodePointCount(RewardRef) > 128) throw std::invalid_argument("haiven: rewardRef must be 1 to 128 characters");
			const int Existing = FindSaved(IntegrationId, RewardRef);
			if (Existing >= 0) return SavedIntents[static_cast<std::size_t>(Existing)];
			FSavedRewardClaim Saved;
			Saved.IntegrationId = IntegrationId;
			Saved.RewardRef = RewardRef;
			const auto Pending = PendingIds.find(Key(IntegrationId, RewardRef));
			if (Pending != PendingIds.end()) Saved.ClaimId = Pending->second;
			else Saved.ClaimId = MintClaimId();
			SavedIntents.push_back(Saved);
			return Saved;
		}

		/** Saved intents in the order they were first recorded. */
		const std::vector<FSavedRewardClaim>& Saved() const { return SavedIntents; }

		/**
		 * The saved intents as a JSON array of {claimId, integrationId, rewardRef}.
		 * The host writes this and passes it back to ImportSaved after a restart.
		 * An empty list is `[]`.
		 */
		std::string ExportSaved() const
		{
			FJson Array;
			Array.Kind = FJson::EKind::Array;
			for (const FSavedRewardClaim& Saved : SavedIntents)
			{
				FJson Object = FJson::MakeObject();
				Object.Set("claimId", FJson::MakeString(Saved.ClaimId));
				Object.Set("integrationId", FJson::MakeString(Saved.IntegrationId));
				Object.Set("rewardRef", FJson::MakeString(Saved.RewardRef));
				Array.Items.push_back(std::move(Object));
			}
			return Array.Dump();
		}

		/**
		 * Replaces saved intents with a document from ExportSaved.
		 * A document that is not that array leaves the current intents in place.
		 * The first entry for an integration and reward wins, so a later duplicate
		 * cannot remint the claim id.
		 */
		bool ImportSaved(const std::string& Document, std::string& Error)
		{
			FJson Root;
			if (!FJson::Parse(Document, Root, Error) || Root.Kind != FJson::EKind::Array)
			{
				if (Error.empty()) Error = "saved reward claims must be a JSON array";
				return false;
			}
			std::vector<FSavedRewardClaim> Imported;
			for (const FJson& Item : Root.Items)
			{
				if (Item.Kind != FJson::EKind::Object) continue;
				FSavedRewardClaim Saved;
				Saved.ClaimId = Item.StringOr("claimId");
				Saved.IntegrationId = Item.StringOr("integrationId");
				Saved.RewardRef = Item.StringOr("rewardRef");
				if (Saved.ClaimId.empty() || Saved.IntegrationId.empty() || Saved.RewardRef.empty()) continue;
				if (Utf8CodePointCount(Saved.RewardRef) > 128) continue;
				if (FindIn(Imported, Saved.IntegrationId, Saved.RewardRef) >= 0) continue;
				Imported.push_back(std::move(Saved));
			}
			SavedIntents = std::move(Imported);
			Error.clear();
			return true;
		}

		/**
		 * One POST /users/me/rewards/claims per saved intent, each under its saved
		 * claim id. Does not drop the intent: HandleClaimReply does that when the
		 * reply is final. A retryable reply keeps the id for the next submit.
		 */
		std::vector<Rest::FRestRequest> SubmitSavedRequests()
		{
			std::vector<Rest::FRestRequest> Requests;
			Requests.reserve(SavedIntents.size());
			for (const FSavedRewardClaim& Saved : SavedIntents)
			{
				const std::string K = Key(Saved.IntegrationId, Saved.RewardRef);
				if (PendingIds.find(K) == PendingIds.end()) PendingIds.emplace(K, Saved.ClaimId);
				RestTypes::ClaimRewardRequest Body;
				Body.ClaimId = Saved.ClaimId;
				Body.IntegrationId = Saved.IntegrationId;
				Body.RewardRef = Saved.RewardRef;
				Requests.push_back(Rest::PostUsersMeRewardsClaims(ContractsJson::ToJson(Body)));
			}
			return Requests;
		}

		/** The claim id the next ClaimRequest for this reward will carry, if one is pending. */
		std::optional<std::string> Pending(const std::string& IntegrationId, const std::string& RewardRef) const
		{
			const auto Found = PendingIds.find(Key(IntegrationId, RewardRef));
			if (Found == PendingIds.end()) return std::nullopt;
			return Found->second;
		}

		/**
		 * The reply to a ClaimRequest. A final answer releases the claim id; a
		 * retryable failure keeps it for the next attempt. A reply from a session
		 * invalidated by Clear is ignored and returns no outcome.
		 */
		std::optional<FRewardClaimOutcome> HandleClaimReply(
			std::uint64_t RequestSessionGeneration,
			const std::string& IntegrationId,
			const std::string& RewardRef,
			const std::string& RequestClaimId,
			int Status,
			const std::string& Body)
		{
			if (RequestSessionGeneration != SessionGeneration) return std::nullopt;
			FRewardClaimOutcome Outcome = Classify(Status, Body);
			if (!Outcome.bRetryable)
			{
				const auto Found = PendingIds.find(Key(IntegrationId, RewardRef));
				if (Found != PendingIds.end() && Found->second == RequestClaimId) PendingIds.erase(Found);
				ReleaseSaved(IntegrationId, RewardRef, RequestClaimId);
			}
			return Outcome;
		}

		/** Identifies the signed-in player's pending-claim state. */
		std::uint64_t GetSessionGeneration() const { return SessionGeneration; }

		/** Forgets the pending claim id of a reward the game no longer claims. */
		void Forget(const std::string& IntegrationId, const std::string& RewardRef) { PendingIds.erase(Key(IntegrationId, RewardRef)); }

		/**
		 * Forgets every in-flight claim id: the player signed out. Saved intents
		 * stay. They belong to the install until the server answers, and the next
		 * link must submit the same claim id.
		 */
		void Clear()
		{
			PendingIds.clear();
			++SessionGeneration;
		}

		/**
		 * A game server's grant of one reward to a player, sent with its
		 * integration key, under a claim id it keeps across its own retries.
		 */
		static Rest::FRestRequest GrantRequest(const std::string& UserId, const std::string& RewardRef, const std::string& ClaimId)
		{
			if (UserId.empty() || RewardRef.empty() || ClaimId.empty()) throw std::invalid_argument("haiven: userId, rewardRef and claimId are required");
			RestTypes::ServerClaimRewardRequest Body;
			Body.ClaimId = ClaimId;
			Body.RewardRef = RewardRef;
			Body.UserId = UserId;
			return Rest::PostIntegrationsRewardsClaims(ContractsJson::ToJson(Body));
		}

		/** A game server's confirmation of a claim the player's client made. */
		static Rest::FRestRequest ConfirmRequest(const std::string& ClaimId)
		{
			if (ClaimId.empty()) throw std::invalid_argument("haiven: claimId is required");
			return Rest::PostIntegrationsRewardsClaimsClaimIdConfirm(ClaimId);
		}

		/**
		 * Any reward reply, by status and the error body's finite code. Status 0
		 * is a transport failure. A 2xx whose body is not a RewardClaimResponse
		 * is Failed and retryable: the claim may have been recorded, and a retry
		 * under the same id returns it.
		 */
		static FRewardClaimOutcome Classify(int Status, const std::string& Body)
		{
			FRewardClaimOutcome Outcome;
			Outcome.Status = Status;
			if (Status == 200 || Status == 201)
			{
				RestTypes::RewardClaimResponse Claim;
				std::string Error;
				if (!ContractsJson::FromJson(Body, Claim, Error))
				{
					Outcome.Error = "The reward claim reply could not be read: " + Error;
					Outcome.bRetryable = true;
					return Outcome;
				}
				if (!Claim.ClaimId.has_value() || Claim.ClaimId->empty())
				{
					Outcome.Error = "The reward claim reply did not include a claimId.";
					Outcome.bRetryable = true;
					return Outcome;
				}
				Outcome.Result = Status == 201 ? ERewardClaimResult::Claimed : ERewardClaimResult::AlreadyRecorded;
				Outcome.Confirmation = ConfirmationOf(Claim.Confirmation);
				Outcome.Claim = std::move(Claim);
				return Outcome;
			}
			Outcome.ErrorCode = ErrorCodeOf(Body);
			const std::string& Code = Outcome.ErrorCode;
			if (Status == 403 && Code == "reward_not_eligible") Outcome.Result = ERewardClaimResult::NotEligible;
			else if (Status == 409 && Code == "reward_already_claimed") Outcome.Result = ERewardClaimResult::AlreadyClaimed;
			else if (Status == 409 && Code == "reward_claim_conflict") Outcome.Result = ERewardClaimResult::ClaimConflict;
			else if (Status == 409 && Code == "refund_in_progress") Outcome.Result = ERewardClaimResult::RefundInProgress;
			else if (Status == 404) Outcome.Result = ERewardClaimResult::NotFound;
			else
			{
				Outcome.bRetryable = Status == 0 || Status == 429 || Status >= 500;
				Outcome.Error = Status == 0 ? std::string("The reward request did not reach the service.")
											: "The reward request failed with HTTP " + std::to_string(Status) + ".";
			}
			return Outcome;
		}

		static ERewardConfirmation ConfirmationOf(const std::optional<std::string>& Confirmation)
		{
			if (Confirmation == "client_reported") return ERewardConfirmation::ClientReported;
			if (Confirmation == "server_confirmed") return ERewardConfirmation::ServerConfirmed;
			return ERewardConfirmation::Unrecognized;
		}

		/** ErrorResponse.code of an error body, or empty when it has none. */
		static std::string ErrorCodeOf(const std::string& Body)
		{
			RestTypes::ErrorResponse Error;
			std::string Ignored;
			if (Body.empty() || !ContractsJson::FromJson(Body, Error, Ignored) || !Error.Code.has_value()) return std::string();
			return *Error.Code;
		}

	private:
		static std::string Key(const std::string& IntegrationId, const std::string& RewardRef) { return IntegrationId + '\n' + RewardRef; }

		static int FindIn(const std::vector<FSavedRewardClaim>& Intents, const std::string& IntegrationId, const std::string& RewardRef)
		{
			for (std::size_t Index = 0; Index < Intents.size(); ++Index)
			{
				if (Intents[Index].IntegrationId == IntegrationId && Intents[Index].RewardRef == RewardRef) return static_cast<int>(Index);
			}
			return -1;
		}

		int FindSaved(const std::string& IntegrationId, const std::string& RewardRef) const
		{
			return FindIn(SavedIntents, IntegrationId, RewardRef);
		}

		std::string MintClaimId()
		{
			std::string Id = NewClaimId();
			if (Id.empty()) throw std::invalid_argument("haiven: claim id source returned an empty id");
			return Id;
		}

		/** The id of an in-flight attempt, else the saved intent, else a new id kept for retry. */
		std::string ClaimIdFor(const std::string& IntegrationId, const std::string& RewardRef)
		{
			const std::string K = Key(IntegrationId, RewardRef);
			const auto Found = PendingIds.find(K);
			if (Found != PendingIds.end()) return Found->second;
			const int Saved = FindSaved(IntegrationId, RewardRef);
			if (Saved >= 0)
			{
				const std::string& Id = SavedIntents[static_cast<std::size_t>(Saved)].ClaimId;
				PendingIds.emplace(K, Id);
				return Id;
			}
			std::string Id = MintClaimId();
			PendingIds.emplace(K, Id);
			return Id;
		}

		void ReleaseSaved(const std::string& IntegrationId, const std::string& RewardRef, const std::string& ClaimId)
		{
			const int Saved = FindSaved(IntegrationId, RewardRef);
			if (Saved < 0) return;
			if (SavedIntents[static_cast<std::size_t>(Saved)].ClaimId != ClaimId) return;
			SavedIntents.erase(SavedIntents.begin() + Saved);
		}

		FClaimIdSource NewClaimId;
		std::map<std::string, std::string> PendingIds;
		std::vector<FSavedRewardClaim> SavedIntents;
		std::uint64_t SessionGeneration = 0;
	};
}
