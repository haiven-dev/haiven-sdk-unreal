// A Haiven client for a C++ host with no engine: the SDK over transports the host provides.
//
// The cores never touch a socket. A host implements two small interfaces over
// whatever networking it already has — a game engine's, a platform SDK's, or a
// library such as libcurl or Boost.Beast — and these clients add what the Unreal
// subsystem adds over IWebSocket and FHttpModule: the URL rules, the headers,
// and the text framing in and out of `FSessionCore`.
//
// Threading is the host's: deliver every transport callback on the one thread
// that also calls these clients (a game's main thread), as the Unreal plugin
// does by dispatching to its game thread. Nothing here locks.
#pragma once

#include "HaivenCredentialLifecycle.h"
#include "HaivenFrames.h"
#include "HaivenRestApi.h"
#include "HaivenSessionCore.h"
#include "HaivenWsApi.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Haiven
{
	struct FHttpRequest
	{
		std::string Method;
		std::string Url;
		std::vector<std::pair<std::string, std::string>> Headers;
		std::string Body;
	};

	struct FHttpResponse
	{
		/// The HTTP status; 0 when no response arrived.
		int Status = 0;
		std::string Body;
		/// Set when the request could not be made or completed; empty otherwise.
		std::string Error;
	};

	using FHttpComplete = std::function<void(const FHttpResponse&)>;

	/// One HTTP exchange. Implement over the host's HTTP stack; call `OnComplete`
	/// exactly once, with `Error` set on a transport failure.
	class IHttpTransport
	{
	public:
		virtual ~IHttpTransport() = default;
		virtual void Send(FHttpRequest Request, FHttpComplete OnComplete) = 0;
	};

	struct FWebSocketEvents
	{
		std::function<void()> OnOpen;
		std::function<void(const std::string&)> OnText;
		/// The close code and reason; a connection that never opened closes too.
		std::function<void(int, const std::string&)> OnClosed;
	};

	/// One text WebSocket. Implement over the host's socket library.
	class IWebSocketTransport
	{
	public:
		virtual ~IWebSocketTransport() = default;
		virtual void Connect(const std::string& Url, FWebSocketEvents Events) = 0;
		virtual void Send(const std::string& Text) = 0;
		virtual void Close(int Code, const std::string& Reason) = 0;
	};

	namespace ClientDetail
	{
		inline bool StartsWithNoCase(const std::string& Value, const std::string& Prefix)
		{
			if (Value.size() < Prefix.size()) return false;
			for (std::size_t Index = 0; Index < Prefix.size(); ++Index)
			{
				char Char = Value[Index];
				if (Char >= 'A' && Char <= 'Z') Char = static_cast<char>(Char - 'A' + 'a');
				if (Char != Prefix[Index]) return false;
			}
			return true;
		}

		/// `scheme://` followed by an exact loopback host, optionally with a port.
		/// The rule the Unreal subsystem applies, so `localhost.example` is remote.
		inline bool IsLoopbackUrl(const std::string& Url, const std::string& Scheme)
		{
			if (!StartsWithNoCase(Url, Scheme)) return false;
			std::string Authority = Url.substr(Scheme.size());
			Authority = Authority.substr(0, Authority.find_first_of("/?#"));
			if (Authority.find('@') != std::string::npos) return false;
			for (const char* Host : {"localhost", "127.0.0.1", "[::1]"})
			{
				const std::string Name = Host;
				if (Authority.size() < Name.size() || !StartsWithNoCase(Authority, Name)) continue;
				if (Authority.size() == Name.size()) return true;
				if (Authority[Name.size()] != ':' || Authority.size() == Name.size() + 1) continue;
				bool bDigits = true;
				for (std::size_t Index = Name.size() + 1; Index < Authority.size(); ++Index)
				{
					bDigits = bDigits && Authority[Index] >= '0' && Authority[Index] <= '9';
				}
				if (bDigits) return true;
			}
			return false;
		}
	}

	/// HTTPS, or HTTP to loopback for development.
	inline bool IsSecureHttpUrl(const std::string& Url)
	{
		return ClientDetail::StartsWithNoCase(Url, "https://") || ClientDetail::IsLoopbackUrl(Url, "http://");
	}

	/// WSS, or WS to loopback for development.
	inline bool IsSecureRealtimeUrl(const std::string& Url)
	{
		return ClientDetail::StartsWithNoCase(Url, "wss://") || ClientDetail::IsLoopbackUrl(Url, "ws://");
	}

	struct FClientOptions
	{
		/// The REST base URL, e.g. `https://api.haiven.gg/v0`.
		std::string BaseUrl;
		/// The realtime endpoint, e.g. `wss://realtime.haiven.gg/v0`.
		std::string RealtimeUrl;
		/// Sent as `X-Haiven-App-ID` when set.
		std::string AppId;
	};

	/// The bearer a request carries; empty means none is available.
	using FAccessTokenSource = std::function<std::string()>;

	/// REST over the host's HTTP: sends what `Haiven::Rest` builds.
	class FRestClient
	{
	public:
		FRestClient(IHttpTransport& InTransport, FClientOptions InOptions, FAccessTokenSource InTokens)
			: Transport(InTransport)
			, Options(std::move(InOptions))
			, Tokens(std::move(InTokens))
		{
			while (!Options.BaseUrl.empty() && Options.BaseUrl.back() == '/') Options.BaseUrl.pop_back();
		}

		/// Sends one built request. Fails through `OnComplete`, without touching
		/// the transport, when the base URL is not secure, the path could select
		/// another authority, no bearer is available, or the operation is a
		/// browser navigation (`bNavigation`), which a host opens with
		/// `Url(Request)` instead.
		void Send(const Rest::FRestRequest& Request, FHttpComplete OnComplete)
		{
			const std::string Token = Tokens ? Tokens() : std::string();
			std::string Error;
			if (Request.bNavigation) Error = "This operation is a browser navigation: open its URL instead of sending it.";
			else if (!IsSecureHttpUrl(Options.BaseUrl)) Error = "Haiven is not configured with a secure absolute base URL.";
			else if (!IsRelativePath(Request.Path)) Error = "A request path must be relative to the base URL.";
			else if (Token.empty()) Error = "No unexpired Haiven access token is available.";
			if (!Error.empty())
			{
				FHttpResponse Failed;
				Failed.Error = std::move(Error);
				OnComplete(Failed);
				return;
			}

			FHttpRequest Http;
			Http.Method = Request.Method;
			Http.Url = Url(Request);
			Http.Headers = {
				{"Accept", "application/json"},
				{"Authorization", "Bearer " + Token},
				{"X-Haiven-SDK", "cpp/0.0.0"},
			};
			if (!Options.AppId.empty()) Http.Headers.emplace_back("X-Haiven-App-ID", Options.AppId);
			if (!Request.ContentType.empty()) Http.Headers.emplace_back("Content-Type", Request.ContentType);
			Http.Body = Request.Body;
			Transport.Send(std::move(Http), std::move(OnComplete));
		}

		/// The absolute URL of a built request.
		std::string Url(const Rest::FRestRequest& Request) const { return Options.BaseUrl + Request.Path; }

	private:
		/// A path never selects an authority: this client adds the bearer to every
		/// request, so `@evil.example/x` after a host-only base URL, or a
		/// scheme-relative `//evil.example`, would hand it to another host. The
		/// same rule as the Unreal subsystem's `ResolveHttpUrl`.
		static bool IsRelativePath(const std::string& Path)
		{
			return !Path.empty() && Path[0] == '/' && Path.compare(0, 2, "//") != 0
				&& Path.find("://") == std::string::npos;
		}

		IHttpTransport& Transport;
		FClientOptions Options;
		FAccessTokenSource Tokens;
	};

	/// The realtime session over the host's WebSocket.
	///
	/// Owns one `FSessionCore` per connection and moves text between it and the
	/// socket: client frames out through `SerializeClientFrame`, server frames in
	/// through `ParseServerFrame`. Set handlers on `Session()` (`OnEvent`,
	/// `OnReply`, `OnStateChanged`, ...) after `Connect`, which starts a fresh one.
	class FRealtimeClient
	{
	public:
		using FRequestIdSource = FSessionCore::FRequestIdSource;

		FRealtimeClient(IWebSocketTransport& InTransport, FClientOptions InOptions, FAccessTokenSource InTokens,
			FRequestIdSource InRequestIds)
			: Transport(InTransport)
			, Options(std::move(InOptions))
			, Tokens(std::move(InTokens))
			, RequestIds(std::move(InRequestIds))
		{
		}

		/// Opens the socket. False, without connecting, when the realtime URL is
		/// not secure or no bearer is available to authenticate with.
		bool Connect(std::string& OutError)
		{
			if (!IsSecureRealtimeUrl(Options.RealtimeUrl))
			{
				OutError = "The realtime URL must use WSS (loopback WS is allowed for development).";
				return false;
			}
			if (!Tokens || Tokens().empty())
			{
				OutError = "No unexpired Haiven access token is available.";
				return false;
			}

			const std::uint64_t Connection = ++Generation;
			Core = std::make_unique<FSessionCore>(
				[this](const FClientFrame& Frame) { Write(Frame); },
				Tokens,
				RequestIds);

			FWebSocketEvents Events;
			// A socket from an earlier `Connect` may still deliver; only the
			// current one reaches the current session.
			Events.OnOpen = [this, Connection] { if (Connection == Generation && Core) Core->Opened(); };
			Events.OnText = [this, Connection](const std::string& Text) { if (Connection == Generation) Receive(Text); };
			Events.OnClosed = [this, Connection](int Code, const std::string& Reason)
			{
				if (Connection == Generation && Core) Core->Closed(Code, Reason);
			};
			Transport.Connect(Options.RealtimeUrl, std::move(Events));
			return true;
		}

		/// The current connection's session; `Connect` first.
		FSessionCore& Session()
		{
			if (!Core) throw std::logic_error("haiven: connect before using the realtime session");
			return *Core;
		}

		bool IsConnected() const { return Core != nullptr && Core->GetState() != ESessionState::Closed; }

		/// Sends a request, queued until the session is ready; returns its requestId.
		std::string Send(std::string Type, std::optional<std::string> PayloadJson = std::nullopt)
		{
			return Session().Send(std::move(Type), std::move(PayloadJson));
		}

		/// Sends a request the generated builders made (`Haiven::Ws::ChatSend`, ...).
		std::string Send(const Ws::FWsRequest& Request) { return Send(Request.Type, Request.PayloadJson); }

		void Close(int Code = 1000, const std::string& Reason = std::string())
		{
			if (!Core) return;
			Transport.Close(Code, Reason);
			Core->Closed(Code, Reason);
		}

		/// Text the socket delivered that is not a frame. Dropped either way; this
		/// only makes it visible.
		std::function<void(const std::string&)> OnMalformedFrame;

		/// A client frame the session wrote whose payload is not JSON, so it was
		/// not sent.
		std::function<void(const FClientFrame&)> OnUnsendableFrame;

	private:
		void Write(const FClientFrame& Frame)
		{
			std::string Text;
			if (!SerializeClientFrame(Frame, Text))
			{
				if (OnUnsendableFrame) OnUnsendableFrame(Frame);
				return;
			}
			Transport.Send(Text);
		}

		void Receive(const std::string& Text)
		{
			if (!Core) return;
			FServerFrame Frame;
			if (!ParseServerFrame(Text, Frame))
			{
				if (OnMalformedFrame) OnMalformedFrame(Text);
				return;
			}
			Core->Receive(Frame, Text);
		}

		IWebSocketTransport& Transport;
		FClientOptions Options;
		FAccessTokenSource Tokens;
		FRequestIdSource RequestIds;
		std::unique_ptr<FSessionCore> Core;
		std::uint64_t Generation = 0;
	};
}
