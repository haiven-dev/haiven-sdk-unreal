#pragma once

#include "Contracts/AccountIdentityLinkFailedV0.h"
#include "Contracts/AccountIdentityLinkedV0.h"
#include "Contracts/MotdV0.h"
#include "HaivenJson.h"
#include "HaivenWsApi.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Haiven
{
	struct FSignInIdentity
	{
		std::string Provider;
		std::string Email;
	};

	struct FSignInMethods
	{
		bool bHasPassword = false;
		std::vector<FSignInIdentity> Identities;
	};

	struct FIdentityReadTicket
	{
		enum class EDomain
		{
			Motd,
			SignInMethods,
		};

		std::uint64_t LifecycleGeneration = 0;
		std::uint64_t Revision = 0;
		std::uint64_t RequestEpoch = 0;
		EDomain Domain = EDomain::SignInMethods;
	};

	/**
	 * Account-scoped guard for a write completion. Reads also carry a domain
	 * revision so newer snapshots/events can supersede them; mutations must not,
	 * because an unrelated read completing first does not undo the write.
	 */
	struct FIdentityMutationTicket
	{
		std::uint64_t LifecycleGeneration = 0;
	};

	enum class EIdentityEventKind
	{
		Ignored,
		Motd,
		Linked,
		LinkFailed,
	};

	struct FIdentityEvent
	{
		EIdentityEventKind Kind = EIdentityEventKind::Ignored;
		bool bValid = false;
		bool bChanged = false;
		std::string Error;
		std::optional<MotdV0> Motd;
		std::optional<AccountIdentityLinkedV0> Linked;
		std::optional<AccountIdentityLinkFailedV0> LinkFailed;
	};

	/**
	 * Engine-neutral identity/MOTD view model.
	 *
	 * Identity linking deliberately does not expose a REST-send callback. The
	 * backend operation establishes browser cookie state and redirects through
	 * federation, so a game HTTP POST followed by opening a system URL is not an
	 * equivalent flow. A host must inject a browser-context adapter which first
	 * prepares a window/context (important for popup-gated hosts), then performs
	 * POST navigation and follows redirects in that same cookie jar.
	 */
	class FIdentityCore
	{
	public:
		using FPrepareBrowser = std::function<bool(std::string& OutContextId, std::string& OutError)>;
		using FPostInBrowser = std::function<bool(
			const std::string& ContextId,
			const std::string& Url,
			std::string& OutError)>;
		using FCloseBrowser = std::function<void(const std::string& ContextId)>;

		FIdentityCore(
			FPrepareBrowser InPrepareBrowser = {},
			FPostInBrowser InPostInBrowser = {},
			FCloseBrowser InCloseBrowser = {})
			: PrepareBrowser(std::move(InPrepareBrowser))
			, PostInBrowser(std::move(InPostInBrowser))
			, CloseBrowser(std::move(InCloseBrowser))
		{
		}

		~FIdentityCore() { CancelLink(); }

		FIdentityCore(const FIdentityCore&) = delete;
		FIdentityCore& operator=(const FIdentityCore&) = delete;

		bool HasBrowserContextAdapter() const
		{
			return static_cast<bool>(PrepareBrowser)
				&& static_cast<bool>(PostInBrowser)
				&& static_cast<bool>(CloseBrowser);
		}

		void SetBrowserContextAdapter(
			FPrepareBrowser InPrepareBrowser,
			FPostInBrowser InPostInBrowser,
			FCloseBrowser InCloseBrowser)
		{
			CancelLink();
			PrepareBrowser = std::move(InPrepareBrowser);
			PostInBrowser = std::move(InPostInBrowser);
			CloseBrowser = std::move(InCloseBrowser);
		}

		static bool IsProvider(const std::string& Provider)
		{
			return Provider == "google" || Provider == "apple";
		}

		/** Prepare a browser before starting the same-context POST navigation. */
		bool BeginLink(const std::string& BaseUrl, const std::string& Provider, std::string& OutError)
		{
			OutError.clear();
			if (!IsProvider(Provider)) return Fail(OutError, "haiven: identity provider must be google or apple");
			if (!IsSecureBaseUrl(BaseUrl)) return Fail(OutError, "haiven: identity linking requires an https base URL (loopback http is allowed for development)");
			if (!HasBrowserContextAdapter()) return Fail(OutError, "haiven: identity linking requires a browser-context adapter");

			CancelLink();
			std::string ContextId;
			if (!PrepareBrowser(ContextId, OutError) || ContextId.empty())
			{
				// A popup-gated host can create the context and then fail while
				// initializing it. Never leak a context it handed back to us.
				if (!ContextId.empty()) CloseBrowser(ContextId);
				if (OutError.empty()) OutError = "haiven: browser context preparation failed";
				return false;
			}

			const std::string Url = TrimTrailingSlash(BaseUrl)
				+ "/users/me/sign-in-methods/sso/" + Provider;
			if (!PostInBrowser(ContextId, Url, OutError))
			{
				CloseBrowser(ContextId);
				if (OutError.empty()) OutError = "haiven: browser-context POST could not be started";
				return false;
			}

			ActiveBrowserContext = std::move(ContextId);
			ActiveProvider = Provider;
			return true;
		}

		void CancelLink()
		{
			if (ActiveBrowserContext && CloseBrowser) CloseBrowser(*ActiveBrowserContext);
			ActiveBrowserContext.reset();
			ActiveProvider.reset();
		}

		bool HasActiveLink() const { return ActiveBrowserContext.has_value(); }
		const std::optional<std::string>& GetActiveProvider() const { return ActiveProvider; }
		FIdentityReadTicket BeginMotdRead()
		{
			return {LifecycleGeneration, MotdRevision, ++MotdRequestEpoch, FIdentityReadTicket::EDomain::Motd};
		}
		FIdentityReadTicket BeginSignInMethodsRead()
		{
			return {LifecycleGeneration, IdentityRevision, ++IdentityRequestEpoch, FIdentityReadTicket::EDomain::SignInMethods};
		}
		bool IsCurrent(const FIdentityReadTicket& Ticket) const { return TicketCurrent(Ticket); }
		FIdentityMutationTicket BeginAccountMutation() const { return {LifecycleGeneration}; }
		bool IsCurrent(const FIdentityMutationTicket& Ticket) const
		{
			return Ticket.LifecycleGeneration == LifecycleGeneration;
		}
		/** Supersede sign-in-method reads started before a successful mutation. */
		void InvalidateSignInMethodsReads() { ++IdentityRevision; }
		static bool IsValidMotdActionUrl(const std::string& Value) { return IsHttpsUrl(Value); }

		/**
		 * A login/logout boundary invalidates every earlier account-owned REST
		 * completion. The public MOTD read is not account-owned and survives it.
		 */
		void ResetAccount()
		{
			++LifecycleGeneration;
			++IdentityRevision;
			SignInMethods.reset();
			CancelLink();
		}

		bool CompleteMotdRead(
			const FIdentityReadTicket& Ticket,
			int StatusCode,
			const std::string& Body,
			bool& OutChanged,
			std::string& OutError)
		{
			OutChanged = false;
			OutError.clear();
			if (!TicketCurrent(Ticket)) return Fail(OutError, "haiven: stale MOTD response ignored");
			++MotdRequestEpoch;
			if (StatusCode == 204 || (StatusCode >= 200 && StatusCode < 300 && Body.empty()))
			{
				bMotdReadFailed = false;
				OutChanged = Motd.has_value();
				if (!OutChanged) return true;
				Motd.reset();
				++MotdRevision;
				return true;
			}
			if (StatusCode < 200 || StatusCode >= 300)
			{
				bMotdReadFailed = true;
				return HttpFailure(StatusCode, OutError);
			}

			MotdV0 Parsed;
			if (!ParseMotd(Body, Parsed, OutError))
			{
				bMotdReadFailed = true;
				return false;
			}
			const bool bRecovering = bMotdReadFailed;
			bMotdReadFailed = false;
			if ((DismissedMotdId && *DismissedMotdId == Parsed.Id)
				|| (Motd && Motd->Id == Parsed.Id))
			{
				OutChanged = bRecovering && !(DismissedMotdId && *DismissedMotdId == Parsed.Id);
				return true;
			}
			OutChanged = true;
			Motd = std::move(Parsed);
			++MotdRevision;
			return true;
		}

		bool CompleteSignInMethodsRead(
			const FIdentityReadTicket& Ticket,
			int StatusCode,
			const std::string& Body,
			bool& OutChanged,
			std::string& OutError)
		{
			OutChanged = false;
			OutError.clear();
			if (!TicketCurrent(Ticket)) return Fail(OutError, "haiven: stale sign-in-method response ignored");
			++IdentityRequestEpoch;
			if (StatusCode < 200 || StatusCode >= 300) return HttpFailure(StatusCode, OutError);
			FSignInMethods Parsed;
			if (!ParseSignInMethods(Body, Parsed, OutError)) return false;
			OutChanged = !SignInMethods || !Same(*SignInMethods, Parsed);
			SignInMethods = std::move(Parsed);
			++IdentityRevision;
			return true;
		}

		/** Apply a validated realtime domain event before older REST completions. */
		FIdentityEvent HandleRealtimeEvent(const std::string& Type, const std::string& EnvelopeJson)
		{
			FIdentityEvent Result;
			if (Type == Ws::Types::Motd) Result.Kind = EIdentityEventKind::Motd;
			else if (Type == Ws::Types::AccountIdentityLinked) Result.Kind = EIdentityEventKind::Linked;
			else if (Type == Ws::Types::AccountIdentityLinkFailed) Result.Kind = EIdentityEventKind::LinkFailed;
			else return Result;

			FJson Payload;
			if (!ExtractEventPayload(Type, EnvelopeJson, Payload, Result.Error)) return Result;

			if (Result.Kind == EIdentityEventKind::Motd)
			{
				MotdV0 Parsed;
				if (!ParseMotd(Payload, Parsed, Result.Error)) return Result;
				const bool bDismissed = DismissedMotdId && *DismissedMotdId == Parsed.Id;
				const bool bDuplicate = Motd && Motd->Id == Parsed.Id;
				Result.bChanged = !bDismissed && (!bDuplicate || bMotdReadFailed);
				if (Result.bChanged)
				{
					Motd = Parsed;
					bMotdReadFailed = false;
				}
				Result.Motd = std::move(Parsed);
			}
			else if (Result.Kind == EIdentityEventKind::Linked)
			{
				AccountIdentityLinkedV0 Parsed;
				if (!ReadLinked(Payload, Parsed, Result.Error)) return Result;
				Result.Linked = Parsed;
				FinishMatchingLink(Parsed.Provider);
				Result.bChanged = true;
			}
			else
			{
				AccountIdentityLinkFailedV0 Parsed;
				if (!ReadLinkFailed(Payload, Parsed, Result.Error)) return Result;
				Result.LinkFailed = Parsed;
				FinishMatchingLink(Parsed.Provider);
				Result.bChanged = true;
			}

			if (Result.Kind == EIdentityEventKind::Motd)
			{
				if (Result.bChanged)
				{
					++MotdRevision;
					++MotdRequestEpoch;
				}
			}
			else ++IdentityRevision;
			Result.bValid = true;
			return Result;
		}

		const std::optional<MotdV0>& GetMotd() const { return Motd; }
		const MotdV0* GetVisibleMotd() const
		{
			if (!Motd || (DismissedMotdId && *DismissedMotdId == Motd->Id)) return nullptr;
			return &*Motd;
		}
		bool DismissMotd()
		{
			const MotdV0* Visible = GetVisibleMotd();
			if (Visible == nullptr) return false;
			DismissedMotdId = Visible->Id;
			++MotdRevision;
			++MotdRequestEpoch;
			return true;
		}
		const std::optional<FSignInMethods>& GetSignInMethods() const { return SignInMethods; }

	private:
		static bool Fail(std::string& OutError, const char* Message)
		{
			OutError = Message;
			return false;
		}

		static bool HttpFailure(int StatusCode, std::string& OutError)
		{
			OutError = "haiven: identity request failed with HTTP " + std::to_string(StatusCode);
			return false;
		}

		static std::string TrimTrailingSlash(std::string Url)
		{
			while (!Url.empty() && Url.back() == '/') Url.pop_back();
			return Url;
		}

		static bool IsLoopbackAuthority(const std::string& Authority)
		{
			const std::size_t Port = Authority.rfind(':');
			const std::string Host = (Port == std::string::npos || Authority == "[::1]")
				? Authority : Authority.substr(0, Port);
			return Host == "localhost" || Host == "127.0.0.1" || Host == "[::1]";
		}

		static bool IsHex(char Value)
		{
			return (Value >= '0' && Value <= '9') || (Value >= 'a' && Value <= 'f')
				|| (Value >= 'A' && Value <= 'F');
		}

		static bool IsValidPort(const std::string& Value)
		{
			if (Value.empty() || Value.size() > 5) return false;
			unsigned Port = 0;
			for (char Digit : Value)
			{
				if (Digit < '0' || Digit > '9') return false;
				Port = Port * 10 + static_cast<unsigned>(Digit - '0');
			}
			return Port > 0 && Port <= 65535;
		}

		static bool IsValidIpv4(const std::string& Value)
		{
			unsigned Parts = 0;
			std::size_t Start = 0;
			while (Start < Value.size())
			{
				const std::size_t End = Value.find('.', Start);
				const std::string Part = Value.substr(Start, End == std::string::npos ? End : End - Start);
				if (Part.empty() || Part.size() > 3 || (Part.size() > 1 && Part[0] == '0')) return false;
				unsigned Number = 0;
				for (char Digit : Part)
				{
					if (Digit < '0' || Digit > '9') return false;
					Number = Number * 10 + static_cast<unsigned>(Digit - '0');
				}
				if (Number > 255) return false;
				++Parts;
				if (End == std::string::npos) break;
				Start = End + 1;
			}
			return Parts == 4;
		}

		static bool CountIpv6Units(const std::string& Value, bool bAllowIpv4, unsigned& OutUnits)
		{
			if (Value.empty()) return true;
			std::size_t Start = 0;
			while (Start <= Value.size())
			{
				const std::size_t End = Value.find(':', Start);
				const std::string Part = Value.substr(Start, End == std::string::npos ? End : End - Start);
				if (Part.empty()) return false;
				if (Part.find('.') != std::string::npos)
				{
					if (!bAllowIpv4 || End != std::string::npos || !IsValidIpv4(Part)) return false;
					OutUnits += 2;
				}
				else
				{
					if (Part.size() > 4) return false;
					for (char Digit : Part) if (!IsHex(Digit)) return false;
					++OutUnits;
				}
				if (End == std::string::npos) break;
				Start = End + 1;
			}
			return true;
		}

		static bool IsValidIpv6(const std::string& Value)
		{
			if (Value.empty()) return false;
			const std::size_t Compression = Value.find("::");
			unsigned Units = 0;
			if (Compression == std::string::npos)
			{
				return CountIpv6Units(Value, true, Units) && Units == 8;
			}
			if (Value.find("::", Compression + 2) != std::string::npos) return false;
			const std::string Left = Value.substr(0, Compression);
			const std::string Right = Value.substr(Compression + 2);
			if (!CountIpv6Units(Left, false, Units) || !CountIpv6Units(Right, true, Units)) return false;
			// `::` stands for at least one omitted 16-bit unit. Requiring fewer
			// than eight explicit units also rejects `:::`, `1:::2`, and trailing
			// or leading single-colon fragments in either section.
			return Units < 8;
		}

		static bool IsValidAuthority(const std::string& Authority)
		{
			if (Authority.empty() || Authority.find('@') != std::string::npos) return false;
			for (char Value : Authority)
				if (static_cast<unsigned char>(Value) <= 0x20 || Value == 0x7f || Value == '\\') return false;
			if (Authority[0] == '[')
			{
				const std::size_t Close = Authority.find(']');
				if (Close == std::string::npos || !IsValidIpv6(Authority.substr(1, Close - 1))) return false;
				if (Close + 1 == Authority.size()) return true;
				return Authority[Close + 1] == ':' && IsValidPort(Authority.substr(Close + 2));
			}

			const std::size_t Colon = Authority.rfind(':');
			if (Colon != std::string::npos && Authority.find(':') != Colon) return false;
			const std::string Host = Authority.substr(0, Colon);
			if (Host.empty() || (Colon != std::string::npos && !IsValidPort(Authority.substr(Colon + 1)))) return false;
			std::size_t LabelStart = 0;
			while (LabelStart < Host.size())
			{
				const std::size_t LabelEnd = Host.find('.', LabelStart);
				const std::size_t Length = (LabelEnd == std::string::npos ? Host.size() : LabelEnd) - LabelStart;
				if (Length == 0 || Length > 63) return false;
				for (std::size_t Index = LabelStart; Index < LabelStart + Length; ++Index)
				{
					const char Value = Host[Index];
					const bool bAlphaNumeric = (Value >= 'a' && Value <= 'z') || (Value >= 'A' && Value <= 'Z')
						|| (Value >= '0' && Value <= '9');
					if (!bAlphaNumeric && Value != '-') return false;
					if ((Index == LabelStart || Index + 1 == LabelStart + Length) && Value == '-') return false;
				}
				if (LabelEnd == std::string::npos) break;
				LabelStart = LabelEnd + 1;
			}
			return Host.size() <= 253;
		}

		static bool IsSecureBaseUrl(const std::string& Url)
		{
			const bool bHttps = Url.rfind("https://", 0) == 0;
			const bool bHttp = Url.rfind("http://", 0) == 0;
			if (!bHttps && !bHttp) return false;
			const std::size_t Start = bHttps ? 8 : 7;
			for (char Value : Url)
				if (static_cast<unsigned char>(Value) <= 0x20 || Value == 0x7f || Value == '\\') return false;
			if (Url.find_first_of("?#", Start) != std::string::npos) return false;
			const std::size_t End = Url.find_first_of("/?#", Start);
			const std::string Authority = Url.substr(Start, End == std::string::npos ? End : End - Start);
			if (!IsValidAuthority(Authority)) return false;
			return bHttps || IsLoopbackAuthority(Authority);
		}

		bool TicketCurrent(const FIdentityReadTicket& Ticket) const
		{
			// The MOTD is public and platform-wide, read without a bearer, so an
			// account boundary does not make a read stale. Only a newer read, a
			// realtime MOTD event or a dismissal supersedes it.
			if (Ticket.Domain == FIdentityReadTicket::EDomain::Motd)
				return Ticket.Revision == MotdRevision && Ticket.RequestEpoch == MotdRequestEpoch;
			if (Ticket.LifecycleGeneration != LifecycleGeneration) return false;
			return Ticket.Revision == IdentityRevision && Ticket.RequestEpoch == IdentityRequestEpoch;
		}

		static bool RequiredString(const FJson& Object, const char* Name, std::string& Out, std::string& Error)
		{
			const FJson* Value = Object.Find(Name);
			if (Value == nullptr || Value->Kind != FJson::EKind::String || Value->String.empty())
			{
				Error = std::string("haiven: expected non-empty string at ") + Name;
				return false;
			}
			Out = Value->String;
			return true;
		}

		static bool RequiredText(const FJson& Object, const char* Name, std::string& Out, std::string& Error)
		{
			const FJson* Value = Object.Find(Name);
			if (Value == nullptr || Value->Kind != FJson::EKind::String)
			{
				Error = std::string("haiven: expected string at ") + Name;
				return false;
			}
			Out = Value->String;
			return true;
		}

		static bool OptionalString(const FJson& Object, const char* Name, std::optional<std::string>& Out, std::string& Error)
		{
			const FJson* Value = Object.Find(Name);
			if (Value == nullptr)
			{
				Out.reset();
				return true;
			}
			if (Value->Kind != FJson::EKind::String)
			{
				Error = std::string("haiven: expected non-empty string or null at ") + Name;
				return false;
			}
			Out = Value->String;
			return true;
		}

		static bool HasOnlyProperties(
			const FJson& Object,
			std::initializer_list<const char*> Allowed,
			std::string& Error)
		{
			for (std::size_t Index = 0; Index < Object.Members.size(); ++Index)
			{
				const std::string& Name = Object.Members[Index].first;
				bool bAllowed = false;
				for (const char* Candidate : Allowed) if (Name == Candidate) { bAllowed = true; break; }
				if (!bAllowed)
				{
					Error = "haiven: unexpected property " + Name;
					return false;
				}
				for (std::size_t Previous = 0; Previous < Index; ++Previous)
					if (Object.Members[Previous].first == Name) return Fail(Error, "haiven: duplicate property");
			}
			return true;
		}

		static bool IsSafeSingleLine(const std::string& Value)
		{
			for (unsigned char Character : Value)
				if (Character < 0x20 || Character == 0x7f) return false;
			return true;
		}

		static bool IsUuid(const std::string& Value)
		{
			if (Value.size() != 36) return false;
			for (std::size_t Index = 0; Index < Value.size(); ++Index)
			{
				const bool bHyphen = Index == 8 || Index == 13 || Index == 18 || Index == 23;
				if (bHyphen ? Value[Index] != '-' : !IsHex(Value[Index])) return false;
			}
			return true;
		}

		static bool LooksLikeTimestamp(const std::string& Value)
		{
			auto Digits = [&Value](std::size_t Start, std::size_t Count)
			{
				if (Start + Count > Value.size()) return false;
				for (std::size_t Index = Start; Index < Start + Count; ++Index)
					if (Value[Index] < '0' || Value[Index] > '9') return false;
				return true;
			};
			auto Pair = [&Value](std::size_t Start)
			{
				return (Value[Start] - '0') * 10 + (Value[Start + 1] - '0');
			};
			auto Year = [&Value]()
			{
				return (Value[0] - '0') * 1000 + (Value[1] - '0') * 100
					+ (Value[2] - '0') * 10 + (Value[3] - '0');
			};
			if (Value.size() < 20 || !Digits(0, 4) || !Digits(5, 2) || !Digits(8, 2)
				|| !Digits(11, 2) || !Digits(14, 2) || !Digits(17, 2)
				|| Value[4] != '-' || Value[7] != '-' || (Value[10] != 'T' && Value[10] != 't')
				|| Value[13] != ':' || Value[16] != ':') return false;
			const int CalendarYear = Year();
			const int Month = Pair(5);
			const int Day = Pair(8);
			if (CalendarYear < 1 || Month < 1 || Month > 12
				|| Pair(11) > 23 || Pair(14) > 59 || Pair(17) > 60) return false;
			static const int DaysByMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
			const bool bLeap = (CalendarYear % 4 == 0 && CalendarYear % 100 != 0) || CalendarYear % 400 == 0;
			const int Days = Month == 2 && bLeap ? 29 : DaysByMonth[Month];
			if (Day < 1 || Day > Days) return false;
			std::size_t Zone = 19;
			if (Value[Zone] == '.')
			{
				const std::size_t Fraction = ++Zone;
				while (Zone < Value.size() && Value[Zone] >= '0' && Value[Zone] <= '9') ++Zone;
				if (Zone == Fraction) return false;
			}
			if (Zone == Value.size() - 1 && (Value[Zone] == 'Z' || Value[Zone] == 'z')) return true;
			return Zone + 6 == Value.size() && (Value[Zone] == '+' || Value[Zone] == '-')
				&& Digits(Zone + 1, 2) && Value[Zone + 3] == ':' && Digits(Zone + 4, 2)
				&& Pair(Zone + 1) <= 23 && Pair(Zone + 4) <= 59;
		}

		static bool IsHttpsUrl(const std::string& Value)
		{
			if (Value.rfind("https://", 0) != 0) return false;
			for (char Character : Value)
				if (static_cast<unsigned char>(Character) <= 0x20 || Character == 0x7f || Character == '\\') return false;
			const std::size_t End = Value.find_first_of("/?#", 8);
			const std::string Authority = Value.substr(8, End == std::string::npos ? End : End - 8);
			return IsValidAuthority(Authority);
		}

		static bool ParseMotd(const std::string& Body, MotdV0& Out, std::string& Error)
		{
			FJson Json;
			if (!FJson::Parse(Body, Json, Error))
			{
				Error = "haiven: invalid MOTD JSON: " + Error;
				return false;
			}
			return ParseMotd(Json, Out, Error);
		}

		static bool ParseMotd(const FJson& Json, MotdV0& Out, std::string& Error)
		{
			if (!Json.IsObject()) return Fail(Error, "haiven: MOTD payload must be an object");
			if (!HasOnlyProperties(Json, {"id", "title", "body", "url", "startsAt", "endsAt"}, Error)) return false;
			if (!RequiredString(Json, "id", Out.Id, Error)
				|| !OptionalString(Json, "title", Out.Title, Error)
				|| !RequiredText(Json, "body", Out.Body, Error)
				|| !OptionalString(Json, "url", Out.Url, Error)
				|| !RequiredString(Json, "startsAt", Out.StartsAt, Error)
				|| !OptionalString(Json, "endsAt", Out.EndsAt, Error)) return false;
			if (!IsUuid(Out.Id)) return Fail(Error, "haiven: MOTD id must be a UUID");
			if (!LooksLikeTimestamp(Out.StartsAt) || (Out.EndsAt && !LooksLikeTimestamp(*Out.EndsAt)))
				return Fail(Error, "haiven: MOTD timestamps must be RFC3339 values");
			// The message remains useful when its optional action is unsafe. The
			// player projection strips that field instead of rejecting the message.
			if (Out.Url && !IsHttpsUrl(*Out.Url)) Out.Url.reset();
			return true;
		}

		static bool ParseSignInMethods(const std::string& Body, FSignInMethods& Out, std::string& Error)
		{
			FJson Json;
			if (!FJson::Parse(Body, Json, Error) || !Json.IsObject())
				return Fail(Error, "haiven: sign-in-method response must be a JSON object");
			const FJson* HasPassword = Json.Find("hasPassword");
			const FJson* Identities = Json.Find("identities");
			if (HasPassword == nullptr || HasPassword->Kind != FJson::EKind::Bool)
				return Fail(Error, "haiven: sign-in-method response requires boolean hasPassword");
			if (Identities == nullptr || Identities->Kind != FJson::EKind::Array)
				return Fail(Error, "haiven: sign-in-method response requires identities array");
			Out.bHasPassword = HasPassword->Bool;
			for (const FJson& Value : Identities->Items)
			{
				if (!Value.IsObject()) return Fail(Error, "haiven: sign-in identity must be an object");
				FSignInIdentity Identity;
				if (!RequiredString(Value, "provider", Identity.Provider, Error)
					|| !RequiredString(Value, "email", Identity.Email, Error)) return false;
				if (!IsProvider(Identity.Provider)) return Fail(Error, "haiven: sign-in identity provider is not supported");
				Out.Identities.push_back(std::move(Identity));
			}
			return true;
		}

		static bool ExtractEventPayload(
			const std::string& Type,
			const std::string& EnvelopeJson,
			FJson& Out,
			std::string& Error)
		{
			FJson Envelope;
			if (!FJson::Parse(EnvelopeJson, Envelope, Error) || !Envelope.IsObject())
				return Fail(Error, "haiven: identity event must be a JSON object");
			// Preserve the stable reply-vs-event diagnostic before reporting any
			// other forbidden envelope member (additionalProperties is still false).
			if (Envelope.Has("success")) return Fail(Error, "haiven: identity event must not carry reply success");
			if (!HasOnlyProperties(Envelope, {"type", "payload"}, Error)) return false;
			const FJson* WireType = Envelope.Find("type");
			if (WireType == nullptr || WireType->Kind != FJson::EKind::String || WireType->String != Type)
				return Fail(Error, "haiven: identity event type does not match its envelope");
			const FJson* Payload = Envelope.Find("payload");
			if (Payload == nullptr || !Payload->IsObject()) return Fail(Error, "haiven: identity event requires an object payload");
			Out = *Payload;
			return true;
		}

		static bool ReadLinked(const FJson& Json, AccountIdentityLinkedV0& Out, std::string& Error)
		{
			if (!HasOnlyProperties(Json, {"provider", "email"}, Error)) return false;
			if (!RequiredString(Json, "provider", Out.Provider, Error)
				|| !RequiredString(Json, "email", Out.Email, Error)) return false;
			if (!IsSafeSingleLine(Out.Provider) || !IsSafeSingleLine(Out.Email))
				return Fail(Error, "haiven: linked identity strings must not contain control characters");
			return IsProvider(Out.Provider) || Fail(Error, "haiven: linked identity provider is not supported");
		}

		static bool ReadLinkFailed(const FJson& Json, AccountIdentityLinkFailedV0& Out, std::string& Error)
		{
			if (!HasOnlyProperties(Json, {"provider", "code"}, Error)) return false;
			if (!RequiredString(Json, "provider", Out.Provider, Error)
				|| !RequiredString(Json, "code", Out.Code, Error)) return false;
			if (!IsSafeSingleLine(Out.Provider) || !IsSafeSingleLine(Out.Code))
				return Fail(Error, "haiven: failed identity strings must not contain control characters");
			return IsProvider(Out.Provider) || Fail(Error, "haiven: failed identity provider is not supported");
		}

		static bool Same(const FSignInMethods& Left, const FSignInMethods& Right)
		{
			if (Left.bHasPassword != Right.bHasPassword || Left.Identities.size() != Right.Identities.size()) return false;
			for (std::size_t Index = 0; Index < Left.Identities.size(); ++Index)
			{
				if (Left.Identities[Index].Provider != Right.Identities[Index].Provider
					|| Left.Identities[Index].Email != Right.Identities[Index].Email) return false;
			}
			return true;
		}

		static bool Same(const MotdV0& Left, const MotdV0& Right)
		{
			return Left.Id == Right.Id
				&& Left.Title == Right.Title
				&& Left.Body == Right.Body
				&& Left.Url == Right.Url
				&& Left.StartsAt == Right.StartsAt
				&& Left.EndsAt == Right.EndsAt;
		}

		void FinishMatchingLink(const std::string& Provider)
		{
			if (ActiveProvider && *ActiveProvider == Provider) CancelLink();
		}

		FPrepareBrowser PrepareBrowser;
		FPostInBrowser PostInBrowser;
		FCloseBrowser CloseBrowser;
		std::optional<std::string> ActiveBrowserContext;
		std::optional<std::string> ActiveProvider;
		std::optional<MotdV0> Motd;
		std::optional<std::string> DismissedMotdId;
		bool bMotdReadFailed = false;
		std::optional<FSignInMethods> SignInMethods;
		std::uint64_t LifecycleGeneration = 1;
		std::uint64_t MotdRevision = 1;
		std::uint64_t IdentityRevision = 1;
		std::uint64_t MotdRequestEpoch = 0;
		std::uint64_t IdentityRequestEpoch = 0;
	};
}
