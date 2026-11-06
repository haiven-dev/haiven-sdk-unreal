#pragma once

#include "Contracts/HaivenRestJson.h"
#include "HaivenRestApi.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace Haiven
{
	/** One host-delivered GET /games/{game_id}/access reply. */
	struct FGameAccessOutcome
	{
		int Status = 0;
		/** Present when a successful response carried a decodable body; empty for 204/no body. */
		std::optional<RestTypes::GameAccessResponse> Access;
		/** ErrorResponse.code, verbatim, including codes minted after this build. */
		std::string ErrorCode;
		/** A diagnostic for transport/decode/HTTP failures. Never branch on this text. */
		std::string Error;

		bool Succeeded() const { return Status >= 200 && Status < 300 && Error.empty(); }
	};

	/** Thin Phase 1 helpers for game access and player statuses. */
	class FPlayerAccess
	{
	public:
		static constexpr const char* Founder = "founder";

		/** Builds the generated game-access request for the host's HTTP transport. */
		static Rest::FRestRequest GameRequest(std::int64_t GameId)
		{
			return Rest::GetGamesGameIdAccess(GameId);
		}

		/**
		 * Decodes a host-delivered reply. Every 2xx with no body succeeds with an
		 * empty Access optional; non-2xx error codes remain open strings.
		 */
		static FGameAccessOutcome HandleGameReply(int Status, const std::string& Body)
		{
			FGameAccessOutcome Outcome;
			Outcome.Status = Status;
			if (Status >= 200 && Status < 300)
			{
				if (Body.empty()) return Outcome;
				RestTypes::GameAccessResponse Access;
				std::string DecodeError;
				if (!ContractsJson::FromJson(Body, Access, DecodeError))
				{
					Outcome.Error = "The game-access reply could not be read: " + DecodeError;
					return Outcome;
				}
				Outcome.Access = std::move(Access);
				return Outcome;
			}

			RestTypes::ErrorResponse ErrorResponse;
			std::string Ignored;
			if (!Body.empty() && ContractsJson::FromJson(Body, ErrorResponse, Ignored) && ErrorResponse.Code)
				Outcome.ErrorCode = *ErrorResponse.Code;
			Outcome.Error = Status == 0
				? std::string("The game-access request did not reach the service.")
				: "The game-access request failed with HTTP " + std::to_string(Status) + ".";
			return Outcome;
		}

		/** Finds an exact open status code without rejecting unknown codes. */
		static std::optional<RestTypes::PlayerStatus> FindStatus(
			const RestTypes::RewardEligibilityResponse& Eligibility,
			const std::string& Code)
		{
			if (Code.empty() || !Eligibility.Statuses) return std::nullopt;
			for (const RestTypes::PlayerStatus& Status : *Eligibility.Statuses)
				if (Status.Code && *Status.Code == Code) return Status;
			return std::nullopt;
		}

		/** The founder status, independent of subscriber eligibility. */
		static std::optional<RestTypes::PlayerStatus> FounderStatus(
			const RestTypes::RewardEligibilityResponse& Eligibility)
		{
			return FindStatus(Eligibility, Founder);
		}
	};
}
