#pragma once

#include <string>

namespace Haiven
{
	enum class ECredentialAction
	{
		Retain,
		Clear,
		RetryFreshSocket,
	};

	enum class ECredentialSocketPhase
	{
		InitialAuthentication,
		Live,
	};

	/** ADR 0041 retention rules shared by the C++ SDK and Unreal adapter. */
	struct FCredentialLifecycle
	{
		static ECredentialAction ForHttpStatus(int Status)
		{
			return Status == 401 ? ECredentialAction::Clear : ECredentialAction::Retain;
		}

		static ECredentialAction ForSocketClose(
			ECredentialSocketPhase Phase,
			int Code,
			const std::string& Reason)
		{
			if (Code != 1008) return ECredentialAction::Retain;
			if (Reason == "credential_revoked") return ECredentialAction::Clear;
			if (Phase == ECredentialSocketPhase::InitialAuthentication && Reason == "unauthorized")
				return ECredentialAction::Clear;
			return ECredentialAction::Retain;
		}

		/**
		 * A live auth.failure refuses only same-socket replacement. ADR 0041 treats
		 * invalid_token and unauthorized as one retry-then-clear rule: close that
		 * socket and retry the credential as first-frame authentication on a fresh
		 * socket, clearing it only if that handshake closes with 1008 unauthorized.
		 */
		static ECredentialAction ForAuthenticationFailure(const std::string& Code)
		{
			return Code == "invalid_token" || Code == "unauthorized"
				? ECredentialAction::RetryFreshSocket
				: ECredentialAction::Retain;
		}
	};
}
