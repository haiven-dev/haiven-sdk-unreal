// The game login handoff (PKCE over a browser redirect), in plain C++17.
//
// The rules a game must get exactly right to redeem a one-use code safely: the
// authorization URL it opens, the callback it accepts, the single use of the
// verifier, the transaction lifetime, and the bearer it may keep. They were
// written against `FString`, `FDateTime`, `FJsonObject` and the engine's
// OpenSSL, so nothing could check them without Unreal. Here they take none of
// those: the Unreal plugin's `Haiven::FHandoffCore` adapts this one, and a plain
// C++ host uses it directly.
//
// The host supplies what only it can: 256 bits of entropy for the state and the
// verifier (from its platform's secure random source), and the current time.
// SHA-256 for the S256 challenge is built in, so no host needs a crypto library
// to sign in; `test/handoff_test.cpp` pins it to the FIPS 180-4 and RFC 7636
// vectors.
#pragma once

#include "HaivenJson.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ratio>
#include <string>
#include <utility>
#include <vector>

namespace Haiven
{
	namespace Handoff
	{
		/// 100 ns ticks since an epoch the host picks and keeps. That is Unreal's
		/// `FDateTime` resolution, so an engine clock converts exactly; a plain host
		/// can pass `std::chrono::system_clock::now().time_since_epoch()`.
		using FTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;

		struct FConfig
		{
			std::string EntryUrl;
			std::string ExchangeUrl;
			std::string IntegrationId;
			std::string RedirectUrl;
			std::string InstructionsId;
			std::string ScreenHint;
			FTicks TransactionLifetime = std::chrono::minutes(15);
		};

		/// What redeeming a callback produces: the form a host POSTs to `ExchangeUrl`.
		struct FExchange
		{
			std::string ExchangeUrl;
			std::string Code;
			std::string CodeVerifier;
			std::string IntegrationId;
			std::string RedirectUrl;

			std::string ToFormBody() const;
		};

		struct FCredential
		{
			std::string AccessToken;
			std::string TokenType;
			/// Meaningful only when `bHasExpiry`.
			FTicks ExpiresAt{};
			/// False for a durable credential, which lasts until it is revoked
			/// (ADR 0041): the exchange returned a null `expires_in`.
			bool bHasExpiry = true;
		};

		/**
		 * Where a host keeps the credential across restarts, so `FCore::Resume`
		 * can reuse it instead of opening the browser. A shipped game supplies one
		 * backed by persistent storage; without one the credential is memory-only,
		 * which suits development and tests.
		 */
		class ICredentialStore
		{
		public:
			virtual ~ICredentialStore() = default;
			/// Writes the saved credential and returns true, or returns false when none is saved.
			virtual bool Load(FCredential& OutCredential) = 0;
			virtual void Store(const FCredential& Credential) = 0;
			virtual void Clear() = 0;
		};

		enum class ECredentialEndReason
		{
			Expired,
			RestUnauthorized,
			WebSocketExpired,
		};

		/// SHA-256 of `Bytes` (FIPS 180-4).
		inline std::array<std::uint8_t, 32> Sha256(const std::string& Bytes);

		/// Base64url without padding (RFC 4648 §5), as PKCE and the state use.
		inline std::string Base64Url(const std::uint8_t* Bytes, std::size_t Count);

		/// RFC 3986 percent-encoding: only unreserved characters stay literal.
		inline std::string UrlEncode(const std::string& Value);

		/// The S256 challenge for a verifier (RFC 7636 §4.2).
		inline std::string PkceChallenge(const std::string& Verifier);

		/**
		 * Target-independent handoff state machine. The verifier is memory-only and
		 * is removed before an exchange can begin, making callback redemption atomic.
		 */
		class FCore
		{
		public:
			FCore() = default;
			FCore(const FCore&) = delete;
			FCore& operator=(const FCore&) = delete;
			~FCore()
			{
				Cancel();
				// Memory only: shutting down is not signing out, so a saved
				// credential stays saved for the next launch.
				ResetCredential();
			}

			/// Keeps every stored credential in `Store` as well, and clears it there.
			/// Not owned; it must outlive this core or be replaced first. Null
			/// returns to memory-only.
			void SetCredentialStore(ICredentialStore* InStore) { Store = InStore; }

			/// Reuses the credential the store saved, so a restart does not send the
			/// player back to the browser. Call it before `Begin`: false means
			/// nothing valid is saved, and the host then begins a handoff. A saved
			/// credential past its expiry is cleared.
			bool Resume(FTicks Now, FCredential& OutCredential);

			/// Starts a transaction: validates the configuration, derives the state
			/// and verifier from 32 bytes of entropy each, and writes the URL to open.
			bool Begin(
				const FConfig& Config,
				const std::vector<std::uint8_t>& StateEntropy,
				const std::vector<std::uint8_t>& VerifierEntropy,
				FTicks Now,
				std::string& OutAuthorizationUrl,
				std::string& OutState,
				std::string& OutError);

			/// Accepts the redirect back. A callback for another redirect or with a
			/// wrong state is rejected without consuming the transaction; any other
			/// outcome consumes it, so a code is redeemed at most once.
			bool ConsumeCallback(
				const std::string& CallbackUrl,
				FTicks Now,
				FExchange& OutExchange,
				std::string& OutError);

			void Cancel();
			bool HasPendingTransaction() const { return !ActiveState.empty(); }

			/// Keeps the bearer from the exchange response, and saves it in the
			/// credential store when one is set. A refresh credential is refused: the
			/// handoff grants a bearer and nothing that renews it.
			bool StoreExchangeResponse(
				const std::string& Json,
				FTicks Now,
				FCredential& OutCredential,
				std::string& OutError);

			bool GetAccessToken(FTicks Now, std::string& OutAccessToken) const;
			/// Erases the credential in memory and in the credential store.
			void ClearCredential();

			/// Clears a bearer ended by metadata expiry, REST 401, or auth.expired,
			/// then starts a new handoff. No web refresh route is involved.
			bool Restart(
				ECredentialEndReason Reason,
				const FConfig& Config,
				const std::vector<std::uint8_t>& StateEntropy,
				const std::vector<std::uint8_t>& VerifierEntropy,
				FTicks Now,
				std::string& OutAuthorizationUrl,
				std::string& OutState,
				std::string& OutError);

		private:
			FConfig ActiveConfig;
			std::string ActiveState;
			std::string ActiveVerifier;
			FTicks CreatedAt{};
			/// A flag beside a plain member, not `std::optional`: GCC's optimiser
			/// reports a false `maybe-uninitialized` on an optional of strings, which
			/// a game building at `-O2 -Werror` would inherit.
			FCredential Credential;
			bool bHasCredential = false;
			ICredentialStore* Store = nullptr;

			void ResetCredential();
		};

		namespace HandoffDetail
		{
			struct FParsedUrl
			{
				std::string Scheme;
				std::string Authority;
				std::string Path;
				/// In document order; a key is unique (`ParseQuery` refuses repeats).
				std::vector<std::pair<std::string, std::string>> Query;

				const std::string* Find(const std::string& Key) const
				{
					for (const auto& Pair : Query)
					{
						if (Pair.first == Key) return &Pair.second;
					}
					return nullptr;
				}

				/// Replaces in place, so a reserved entry parameter keeps its slot.
				void Set(const std::string& Key, std::string Value)
				{
					for (auto& Pair : Query)
					{
						if (Pair.first == Key)
						{
							Pair.second = std::move(Value);
							return;
						}
					}
					Query.emplace_back(Key, std::move(Value));
				}
			};

			inline std::string Lower(std::string Value)
			{
				for (char& Char : Value)
				{
					if (Char >= 'A' && Char <= 'Z') Char = static_cast<char>(Char - 'A' + 'a');
				}
				return Value;
			}

			inline bool StartsWith(const std::string& Value, const std::string& Prefix)
			{
				return Value.compare(0, Prefix.size(), Prefix) == 0;
			}

			inline int HexValue(char Char)
			{
				if (Char >= '0' && Char <= '9') return Char - '0';
				if (Char >= 'a' && Char <= 'f') return Char - 'a' + 10;
				if (Char >= 'A' && Char <= 'F') return Char - 'A' + 10;
				return -1;
			}

			/// `+` is a space in a query, then `%XX` is a byte. A malformed escape
			/// stays literal rather than failing the whole callback.
			inline std::string QueryDecode(const std::string& Value)
			{
				std::string Out;
				Out.reserve(Value.size());
				for (std::size_t Index = 0; Index < Value.size(); ++Index)
				{
					const char Char = Value[Index];
					if (Char == '+')
					{
						Out.push_back(' ');
					}
					else if (Char == '%' && Index + 2 < Value.size() && HexValue(Value[Index + 1]) >= 0
						&& HexValue(Value[Index + 2]) >= 0)
					{
						Out.push_back(static_cast<char>(HexValue(Value[Index + 1]) * 16 + HexValue(Value[Index + 2])));
						Index += 2;
					}
					else
					{
						Out.push_back(Char);
					}
				}
				return Out;
			}

			inline bool ParseQuery(const std::string& Text, FParsedUrl& Out)
			{
				std::size_t Start = 0;
				while (Start <= Text.size())
				{
					std::size_t End = Text.find('&', Start);
					if (End == std::string::npos) End = Text.size();
					const std::string Field = Text.substr(Start, End - Start);
					Start = End + 1;
					if (Field.empty()) continue;
					const std::size_t Equals = Field.find('=');
					std::string Key = QueryDecode(Equals == std::string::npos ? Field : Field.substr(0, Equals));
					std::string Value = Equals == std::string::npos ? std::string() : QueryDecode(Field.substr(Equals + 1));
					if (Key.empty() || Out.Find(Key) != nullptr) return false;
					Out.Query.emplace_back(std::move(Key), std::move(Value));
				}
				return true;
			}

			/// An absolute, fragment-free URL: scheme and authority lower-cased, an
			/// empty path read as `/`, and a query whose keys do not repeat.
			inline bool ParseAbsoluteUrl(const std::string& Url, FParsedUrl& Out)
			{
				Out = FParsedUrl();
				if (Url.find('#') != std::string::npos) return false;
				const std::size_t SchemeEnd = Url.find(':');
				if (SchemeEnd == std::string::npos || SchemeEnd == 0) return false;
				Out.Scheme = Lower(Url.substr(0, SchemeEnd));
				std::size_t Cursor = SchemeEnd + 1;
				if (Url.compare(Cursor, 2, "//") == 0)
				{
					Cursor += 2;
					std::size_t AuthorityEnd = Url.find_first_of("/?", Cursor);
					if (AuthorityEnd == std::string::npos) AuthorityEnd = Url.size();
					Out.Authority = Lower(Url.substr(Cursor, AuthorityEnd - Cursor));
					if (Out.Authority.empty()) return false;
					Cursor = AuthorityEnd;
				}

				const std::size_t QueryStart = Url.find('?', Cursor);
				const std::size_t PathEnd = QueryStart == std::string::npos ? Url.size() : QueryStart;
				Out.Path = Url.substr(Cursor, PathEnd - Cursor);
				if (Out.Path.empty()) Out.Path = "/";
				return QueryStart == std::string::npos || ParseQuery(Url.substr(QueryStart + 1), Out);
			}

			/// HTTPS, or HTTP to an exact loopback host for development. The match is
			/// on the whole host, so `localhost.attacker.example` is not loopback.
			inline bool IsSecureWebUrl(const FParsedUrl& Url)
			{
				const std::string& Authority = Url.Authority;
				const bool bLoopback = Authority == "localhost" || StartsWith(Authority, "localhost:")
					|| Authority == "127.0.0.1" || StartsWith(Authority, "127.0.0.1:")
					|| Authority == "[::1]" || StartsWith(Authority, "[::1]:");
				return Url.Scheme == "https" || (Url.Scheme == "http" && bLoopback);
			}

			inline std::string BuildAuthorizationUrl(
				const FConfig& Config, const std::string& State, const std::string& Challenge)
			{
				FParsedUrl Entry;
				ParseAbsoluteUrl(Config.EntryUrl, Entry);
				Entry.Set("integration_id", Config.IntegrationId);
				Entry.Set("completion", "redirect");
				Entry.Set("redirect_uri", Config.RedirectUrl);
				Entry.Set("state", State);
				Entry.Set("code_challenge", Challenge);
				Entry.Set("code_challenge_method", "S256");
				if (!Config.InstructionsId.empty()) Entry.Set("instructions_id", Config.InstructionsId);
				if (!Config.ScreenHint.empty()) Entry.Set("screen_hint", Config.ScreenHint);

				std::string Result = Config.EntryUrl.substr(0, Config.EntryUrl.find('?'));
				const char* Separator = "?";
				for (const auto& Pair : Entry.Query)
				{
					Result += Separator + UrlEncode(Pair.first) + "=" + UrlEncode(Pair.second);
					Separator = "&";
				}
				return Result;
			}

			/// Overwrites a secret before releasing it. `volatile` keeps the
			/// compiler from eliding stores to memory about to be freed.
			inline void SecureReset(std::string& Value)
			{
				volatile char* Bytes = Value.empty() ? nullptr : &Value[0];
				for (std::size_t Index = 0; Index < Value.size(); ++Index) Bytes[Index] = 0;
				Value.clear();
			}

			inline std::uint32_t RotateRight(std::uint32_t Value, int Bits)
			{
				return (Value >> Bits) | (Value << (32 - Bits));
			}
		}

		inline std::array<std::uint8_t, 32> Sha256(const std::string& Bytes)
		{
			using HandoffDetail::RotateRight;
			static constexpr std::uint32_t K[64] = {
				0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
				0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
				0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
				0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
				0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
				0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
				0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
				0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
			};
			std::uint32_t H[8] = {
				0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
			};

			// The message, a 1 bit, zeros to 56 mod 64, then its length in bits.
			std::vector<std::uint8_t> Message(Bytes.begin(), Bytes.end());
			const std::uint64_t BitLength = static_cast<std::uint64_t>(Bytes.size()) * 8;
			Message.push_back(0x80);
			while (Message.size() % 64 != 56) Message.push_back(0);
			for (int Shift = 56; Shift >= 0; Shift -= 8) Message.push_back(static_cast<std::uint8_t>(BitLength >> Shift));

			for (std::size_t Block = 0; Block < Message.size(); Block += 64)
			{
				std::uint32_t W[64];
				for (int Index = 0; Index < 16; ++Index)
				{
					const std::uint8_t* Word = &Message[Block + static_cast<std::size_t>(Index) * 4];
					W[Index] = (std::uint32_t(Word[0]) << 24) | (std::uint32_t(Word[1]) << 16)
						| (std::uint32_t(Word[2]) << 8) | std::uint32_t(Word[3]);
				}
				for (int Index = 16; Index < 64; ++Index)
				{
					const std::uint32_t S0 = RotateRight(W[Index - 15], 7) ^ RotateRight(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
					const std::uint32_t S1 = RotateRight(W[Index - 2], 17) ^ RotateRight(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
					W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
				}

				std::uint32_t A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
				for (int Index = 0; Index < 64; ++Index)
				{
					const std::uint32_t S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
					const std::uint32_t Choose = (E & F) ^ (~E & G);
					const std::uint32_t Temp1 = Hh + S1 + Choose + K[Index] + W[Index];
					const std::uint32_t S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
					const std::uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
					const std::uint32_t Temp2 = S0 + Majority;
					Hh = G;
					G = F;
					F = E;
					E = D + Temp1;
					D = C;
					C = B;
					B = A;
					A = Temp1 + Temp2;
				}
				H[0] += A;
				H[1] += B;
				H[2] += C;
				H[3] += D;
				H[4] += E;
				H[5] += F;
				H[6] += G;
				H[7] += Hh;
			}

			std::array<std::uint8_t, 32> Digest{};
			for (std::size_t Index = 0; Index < 8; ++Index)
			{
				Digest[Index * 4] = static_cast<std::uint8_t>(H[Index] >> 24);
				Digest[Index * 4 + 1] = static_cast<std::uint8_t>(H[Index] >> 16);
				Digest[Index * 4 + 2] = static_cast<std::uint8_t>(H[Index] >> 8);
				Digest[Index * 4 + 3] = static_cast<std::uint8_t>(H[Index]);
			}
			return Digest;
		}

		inline std::string Base64Url(const std::uint8_t* Bytes, std::size_t Count)
		{
			static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
			std::string Out;
			Out.reserve((Count * 4 + 2) / 3);
			std::size_t Index = 0;
			for (; Index + 2 < Count; Index += 3)
			{
				const std::uint32_t Group = (std::uint32_t(Bytes[Index]) << 16) | (std::uint32_t(Bytes[Index + 1]) << 8) | Bytes[Index + 2];
				Out.push_back(Alphabet[(Group >> 18) & 63]);
				Out.push_back(Alphabet[(Group >> 12) & 63]);
				Out.push_back(Alphabet[(Group >> 6) & 63]);
				Out.push_back(Alphabet[Group & 63]);
			}
			if (Count - Index == 1)
			{
				const std::uint32_t Group = std::uint32_t(Bytes[Index]) << 16;
				Out.push_back(Alphabet[(Group >> 18) & 63]);
				Out.push_back(Alphabet[(Group >> 12) & 63]);
			}
			else if (Count - Index == 2)
			{
				const std::uint32_t Group = (std::uint32_t(Bytes[Index]) << 16) | (std::uint32_t(Bytes[Index + 1]) << 8);
				Out.push_back(Alphabet[(Group >> 18) & 63]);
				Out.push_back(Alphabet[(Group >> 12) & 63]);
				Out.push_back(Alphabet[(Group >> 6) & 63]);
			}
			return Out;
		}

		inline std::string UrlEncode(const std::string& Value)
		{
			static constexpr char Digits[] = "0123456789ABCDEF";
			std::string Out;
			for (const char Char : Value)
			{
				const unsigned char Byte = static_cast<unsigned char>(Char);
				const bool bUnreserved = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z')
					|| (Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~';
				if (bUnreserved)
				{
					Out.push_back(Char);
				}
				else
				{
					Out.push_back('%');
					Out.push_back(Digits[Byte >> 4]);
					Out.push_back(Digits[Byte & 0xF]);
				}
			}
			return Out;
		}

		inline std::string PkceChallenge(const std::string& Verifier)
		{
			const std::array<std::uint8_t, 32> Digest = Sha256(Verifier);
			return Base64Url(Digest.data(), Digest.size());
		}

		inline std::string FExchange::ToFormBody() const
		{
			return "code=" + UrlEncode(Code)
				+ "&code_verifier=" + UrlEncode(CodeVerifier)
				+ "&integration_id=" + UrlEncode(IntegrationId)
				+ "&redirect_uri=" + UrlEncode(RedirectUrl);
		}

		inline bool FCore::Begin(
			const FConfig& Config,
			const std::vector<std::uint8_t>& StateEntropy,
			const std::vector<std::uint8_t>& VerifierEntropy,
			FTicks Now,
			std::string& OutAuthorizationUrl,
			std::string& OutState,
			std::string& OutError)
		{
			using namespace HandoffDetail;
			FParsedUrl Entry;
			FParsedUrl Exchange;
			FParsedUrl Redirect;
			if (!ParseAbsoluteUrl(Config.EntryUrl, Entry) || !IsSecureWebUrl(Entry))
			{
				OutError = "The handoff entry URL must use HTTPS (loopback HTTP is allowed for development).";
				return false;
			}
			if (!ParseAbsoluteUrl(Config.ExchangeUrl, Exchange) || !IsSecureWebUrl(Exchange))
			{
				OutError = "The handoff exchange URL must use HTTPS (loopback HTTP is allowed for development).";
				return false;
			}
			if (!ParseAbsoluteUrl(Config.RedirectUrl, Redirect) || Config.IntegrationId.empty())
			{
				OutError = "An integration id and absolute fragment-free redirect URL are required.";
				return false;
			}
			if (Config.TransactionLifetime <= FTicks::zero() || StateEntropy.size() != 32 || VerifierEntropy.size() != 32)
			{
				OutError = "The transaction lifetime and 256-bit state/verifier entropy are required.";
				return false;
			}

			Cancel();
			ActiveConfig = Config;
			ActiveState = Base64Url(StateEntropy.data(), StateEntropy.size());
			ActiveVerifier = Base64Url(VerifierEntropy.data(), VerifierEntropy.size());
			CreatedAt = Now;
			OutState = ActiveState;
			OutAuthorizationUrl = BuildAuthorizationUrl(Config, ActiveState, PkceChallenge(ActiveVerifier));
			return true;
		}

		inline bool FCore::ConsumeCallback(
			const std::string& CallbackUrl,
			FTicks Now,
			FExchange& OutExchange,
			std::string& OutError)
		{
			using namespace HandoffDetail;
			if (ActiveState.empty())
			{
				OutError = "The handoff state is unknown, expired, cancelled, or already consumed.";
				return false;
			}

			FParsedUrl Expected;
			FParsedUrl Callback;
			if (!ParseAbsoluteUrl(ActiveConfig.RedirectUrl, Expected)
				|| !ParseAbsoluteUrl(CallbackUrl, Callback)
				|| Expected.Scheme != Callback.Scheme
				|| Expected.Authority != Callback.Authority
				|| Expected.Path != Callback.Path)
			{
				OutError = "The handoff callback does not match the configured redirect URL.";
				return false;
			}
			for (const auto& Pair : Expected.Query)
			{
				const std::string* Value = Callback.Find(Pair.first);
				if (Value == nullptr || *Value != Pair.second)
				{
					OutError = "The handoff callback does not preserve the configured redirect query.";
					return false;
				}
			}

			const std::string* State = Callback.Find("state");
			if (State == nullptr || *State != ActiveState)
			{
				OutError = "The handoff callback state is missing or does not match.";
				return false;
			}

			// From here every outcome consumes the transaction.
			std::string VerifierToConsume = std::move(ActiveVerifier);
			SecureReset(ActiveState);
			SecureReset(ActiveVerifier);

			if (const std::string* Error = Callback.Find("error"); Error != nullptr && !Error->empty())
			{
				SecureReset(VerifierToConsume);
				OutError = "The handoff callback failed: " + *Error;
				return false;
			}
			const std::string* Code = Callback.Find("code");
			if (Code == nullptr || Code->empty())
			{
				SecureReset(VerifierToConsume);
				OutError = "The handoff callback has no code.";
				return false;
			}
			if (Now - CreatedAt > ActiveConfig.TransactionLifetime)
			{
				SecureReset(VerifierToConsume);
				OutError = "The handoff transaction has expired.";
				return false;
			}

			OutExchange.ExchangeUrl = ActiveConfig.ExchangeUrl;
			OutExchange.Code = *Code;
			OutExchange.CodeVerifier = std::move(VerifierToConsume);
			OutExchange.IntegrationId = ActiveConfig.IntegrationId;
			OutExchange.RedirectUrl = ActiveConfig.RedirectUrl;
			return true;
		}

		inline void FCore::Cancel()
		{
			HandoffDetail::SecureReset(ActiveState);
			HandoffDetail::SecureReset(ActiveVerifier);
		}

		inline bool FCore::StoreExchangeResponse(
			const std::string& Json,
			FTicks Now,
			FCredential& OutCredential,
			std::string& OutError)
		{
			FJson Object;
			std::string ParseError;
			if (!FJson::Parse(Json, Object, ParseError) || !Object.IsObject())
			{
				OutError = "The handoff exchange returned invalid JSON.";
				return false;
			}
			if (Object.Has("refresh_token"))
			{
				OutError = "The handoff exchange returned a forbidden refresh credential.";
				return false;
			}
			const FJson* AccessToken = Object.Find("access_token");
			const FJson* TokenType = Object.Find("token_type");
			const FJson* ExpiresIn = Object.Find("expires_in");
			// Present but null for a durable credential (ADR 0041).
			const bool bDurable = ExpiresIn != nullptr && ExpiresIn->Kind == FJson::EKind::Null;
			// A finite lifetime is a whole number of seconds (SDK-HANDOFF-11). A
			// lifetime past a century is not a credential policy, and its ticks
			// would overflow; refuse both with the other malformed responses.
			if (AccessToken == nullptr || !AccessToken->IsString() || AccessToken->String.empty()
				|| TokenType == nullptr || !TokenType->IsString() || TokenType->String.empty()
				|| ExpiresIn == nullptr
				|| (!bDurable
					&& (ExpiresIn->Kind != FJson::EKind::Number
						|| !(ExpiresIn->Number >= 0.0) || ExpiresIn->Number > 3.2e9
						|| ExpiresIn->Number != std::floor(ExpiresIn->Number))))
			{
				OutError = "The handoff exchange returned an invalid credential.";
				return false;
			}

			ResetCredential();
			OutCredential = FCredential{AccessToken->String, TokenType->String, FTicks::zero(), !bDurable};
			if (!bDurable)
				OutCredential.ExpiresAt = Now + FTicks(static_cast<std::int64_t>(std::llround(ExpiresIn->Number * 1.0e7)));
			Credential = OutCredential;
			bHasCredential = true;
			if (Store != nullptr) Store->Store(Credential);
			return true;
		}

		inline bool FCore::Resume(FTicks Now, FCredential& OutCredential)
		{
			if (Store == nullptr) return false;
			FCredential Saved;
			if (!Store->Load(Saved)) return false;
			if (Saved.AccessToken.empty() || (Saved.bHasExpiry && Now >= Saved.ExpiresAt))
			{
				HandoffDetail::SecureReset(Saved.AccessToken);
				HandoffDetail::SecureReset(Saved.TokenType);
				ClearCredential();
				return false;
			}
			if (Saved.TokenType.empty()) Saved.TokenType = "Bearer";
			ResetCredential();
			Credential = Saved;
			bHasCredential = true;
			OutCredential = std::move(Saved);
			return true;
		}

		inline bool FCore::GetAccessToken(FTicks Now, std::string& OutAccessToken) const
		{
			if (!bHasCredential || Credential.AccessToken.empty()) return false;
			if (Credential.bHasExpiry && Now >= Credential.ExpiresAt) return false;
			OutAccessToken = Credential.AccessToken;
			return true;
		}

		inline void FCore::ResetCredential()
		{
			HandoffDetail::SecureReset(Credential.AccessToken);
			HandoffDetail::SecureReset(Credential.TokenType);
			Credential.ExpiresAt = FTicks::zero();
			Credential.bHasExpiry = true;
			bHasCredential = false;
		}

		inline void FCore::ClearCredential()
		{
			ResetCredential();
			if (Store != nullptr) Store->Clear();
		}

		inline bool FCore::Restart(
			ECredentialEndReason Reason,
			const FConfig& Config,
			const std::vector<std::uint8_t>& StateEntropy,
			const std::vector<std::uint8_t>& VerifierEntropy,
			FTicks Now,
			std::string& OutAuthorizationUrl,
			std::string& OutState,
			std::string& OutError)
		{
			switch (Reason)
			{
			case ECredentialEndReason::Expired:
			case ECredentialEndReason::RestUnauthorized:
			case ECredentialEndReason::WebSocketExpired:
				break;
			default:
				OutError = "The handoff credential end reason is invalid.";
				return false;
			}
			ClearCredential();
			return Begin(Config, StateEntropy, VerifierEntropy, Now, OutAuthorizationUrl, OutState, OutError);
		}
	}
}
