// A WebSocket and HTTP client in plain C++17 over POSIX sockets.
//
// The engine has both — `IWebSocket` and `FHttpModule` — and the runtime plugin
// uses them. This exists because the conformance runner must be executable on a
// machine with no engine installed, which is every machine this repository is
// checked on: `#sdkunrealeditorci` still owns provisioned engine runners, and a
// runner that could only run where they are would be another file nothing
// compiles (F20).
//
// It is scoped to exactly what the fixture speaks and no further: loopback
// `ws://`, text frames, HTTP/1.1 with an explicit Content-Length. No TLS, no
// redirects, no chunked bodies, no permessage-deflate. Anything the fixture
// sends that is outside that scope is reported as an error rather than ignored,
// because a transport that silently drops what it does not understand would
// make `expectSilence` pass for the wrong reason.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace HaivenConformance
{
	/// SHA-1, only to verify `Sec-WebSocket-Accept`.
	///
	/// Verified rather than ignored: a server that answers the upgrade without
	/// the derived key is not speaking RFC 6455, and the next thing that happens
	/// is a frame parse failure whose message points at framing instead of at
	/// the handshake. Sixty lines to make the error name its own cause.
	inline std::string Sha1(const std::string& Input)
	{
		std::uint32_t State[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
		std::string Message = Input;
		const std::uint64_t BitLength = static_cast<std::uint64_t>(Input.size()) * 8u;
		Message.push_back(static_cast<char>(0x80));
		while (Message.size() % 64 != 56) Message.push_back('\0');
		for (int Index = 7; Index >= 0; --Index)
		{
			Message.push_back(static_cast<char>((BitLength >> (Index * 8)) & 0xFF));
		}

		for (std::size_t Offset = 0; Offset < Message.size(); Offset += 64)
		{
			std::uint32_t Words[80];
			for (int Index = 0; Index < 16; ++Index)
			{
				const unsigned char* Bytes =
					reinterpret_cast<const unsigned char*>(Message.data()) + Offset + Index * 4;
				Words[Index] = (static_cast<std::uint32_t>(Bytes[0]) << 24)
					| (static_cast<std::uint32_t>(Bytes[1]) << 16)
					| (static_cast<std::uint32_t>(Bytes[2]) << 8)
					| static_cast<std::uint32_t>(Bytes[3]);
			}
			for (int Index = 16; Index < 80; ++Index)
			{
				const std::uint32_t Value =
					Words[Index - 3] ^ Words[Index - 8] ^ Words[Index - 14] ^ Words[Index - 16];
				Words[Index] = (Value << 1) | (Value >> 31);
			}

			std::uint32_t A = State[0], B = State[1], C = State[2], D = State[3], E = State[4];
			for (int Index = 0; Index < 80; ++Index)
			{
				std::uint32_t Mix = 0;
				std::uint32_t Constant = 0;
				if (Index < 20)
				{
					Mix = (B & C) | (~B & D);
					Constant = 0x5A827999u;
				}
				else if (Index < 40)
				{
					Mix = B ^ C ^ D;
					Constant = 0x6ED9EBA1u;
				}
				else if (Index < 60)
				{
					Mix = (B & C) | (B & D) | (C & D);
					Constant = 0x8F1BBCDCu;
				}
				else
				{
					Mix = B ^ C ^ D;
					Constant = 0xCA62C1D6u;
				}
				const std::uint32_t Next = ((A << 5) | (A >> 27)) + Mix + E + Constant + Words[Index];
				E = D;
				D = C;
				C = (B << 30) | (B >> 2);
				B = A;
				A = Next;
			}
			State[0] += A;
			State[1] += B;
			State[2] += C;
			State[3] += D;
			State[4] += E;
		}

		std::string Digest;
		Digest.reserve(20);
		for (const std::uint32_t Word : State)
		{
			Digest.push_back(static_cast<char>((Word >> 24) & 0xFF));
			Digest.push_back(static_cast<char>((Word >> 16) & 0xFF));
			Digest.push_back(static_cast<char>((Word >> 8) & 0xFF));
			Digest.push_back(static_cast<char>(Word & 0xFF));
		}
		return Digest;
	}

	inline std::string Base64(const std::string& Input)
	{
		static const char* Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Out;
		Out.reserve(((Input.size() + 2) / 3) * 4);
		std::size_t At = 0;
		while (At + 2 < Input.size())
		{
			const std::uint32_t Triple = (static_cast<unsigned char>(Input[At]) << 16)
				| (static_cast<unsigned char>(Input[At + 1]) << 8)
				| static_cast<unsigned char>(Input[At + 2]);
			Out.push_back(Alphabet[(Triple >> 18) & 0x3F]);
			Out.push_back(Alphabet[(Triple >> 12) & 0x3F]);
			Out.push_back(Alphabet[(Triple >> 6) & 0x3F]);
			Out.push_back(Alphabet[Triple & 0x3F]);
			At += 3;
		}
		if (At + 1 == Input.size())
		{
			const std::uint32_t Triple = static_cast<unsigned char>(Input[At]) << 16;
			Out.push_back(Alphabet[(Triple >> 18) & 0x3F]);
			Out.push_back(Alphabet[(Triple >> 12) & 0x3F]);
			Out += "==";
		}
		else if (At + 2 == Input.size())
		{
			const std::uint32_t Triple = (static_cast<unsigned char>(Input[At]) << 16)
				| (static_cast<unsigned char>(Input[At + 1]) << 8);
			Out.push_back(Alphabet[(Triple >> 18) & 0x3F]);
			Out.push_back(Alphabet[(Triple >> 12) & 0x3F]);
			Out.push_back(Alphabet[(Triple >> 6) & 0x3F]);
			Out.push_back('=');
		}
		return Out;
	}

	struct FUrl
	{
		std::string Scheme;
		std::string Host;
		std::string Port;
		std::string Path = "/";
	};

	inline bool ParseUrl(const std::string& Url, FUrl& Out, std::string& Error)
	{
		const std::size_t SchemeEnd = Url.find("://");
		if (SchemeEnd == std::string::npos)
		{
			Error = "no scheme in " + Url;
			return false;
		}
		Out.Scheme = Url.substr(0, SchemeEnd);
		const std::size_t AuthorityStart = SchemeEnd + 3;
		std::size_t PathStart = Url.find('/', AuthorityStart);
		const std::string Authority = Url.substr(
			AuthorityStart,
			PathStart == std::string::npos ? std::string::npos : PathStart - AuthorityStart);
		Out.Path = PathStart == std::string::npos ? "/" : Url.substr(PathStart);

		const std::size_t Colon = Authority.rfind(':');
		if (Colon == std::string::npos)
		{
			Out.Host = Authority;
			Out.Port = (Out.Scheme == "wss" || Out.Scheme == "https") ? "443" : "80";
		}
		else
		{
			Out.Host = Authority.substr(0, Colon);
			Out.Port = Authority.substr(Colon + 1);
		}
		if (Out.Scheme == "wss" || Out.Scheme == "https")
		{
			// Said out loud rather than attempted. The fixture binds loopback and
			// speaks plaintext; a TLS URL here means the runner was pointed at
			// something else, and pretending to connect would fail later as a
			// protocol error.
			Error = "this runner speaks plaintext only; got " + Url;
			return false;
		}
		if (Out.Host.empty())
		{
			Error = "no host in " + Url;
			return false;
		}
		return true;
	}

	inline int DialTcp(const std::string& Host, const std::string& Port, std::string& Error)
	{
		addrinfo Hints{};
		Hints.ai_family = AF_UNSPEC;
		Hints.ai_socktype = SOCK_STREAM;

		addrinfo* Results = nullptr;
		const int Resolved = getaddrinfo(Host.c_str(), Port.c_str(), &Hints, &Results);
		if (Resolved != 0)
		{
			Error = "could not resolve " + Host + ":" + Port + ": " + gai_strerror(Resolved);
			return -1;
		}

		int Descriptor = -1;
		for (addrinfo* Candidate = Results; Candidate != nullptr; Candidate = Candidate->ai_next)
		{
			Descriptor = socket(Candidate->ai_family, Candidate->ai_socktype, Candidate->ai_protocol);
			if (Descriptor < 0) continue;
			if (connect(Descriptor, Candidate->ai_addr, Candidate->ai_addrlen) == 0) break;
			close(Descriptor);
			Descriptor = -1;
		}
		freeaddrinfo(Results);

		if (Descriptor < 0) Error = "could not connect to " + Host + ":" + Port;
		return Descriptor;
	}

	inline bool WriteAll(int Descriptor, const std::string& Bytes, std::string& Error)
	{
		std::size_t Sent = 0;
		while (Sent < Bytes.size())
		{
			const ssize_t Wrote = send(Descriptor, Bytes.data() + Sent, Bytes.size() - Sent, MSG_NOSIGNAL);
			if (Wrote <= 0)
			{
				if (Wrote < 0 && errno == EINTR) continue;
				Error = std::string("send failed: ") + std::strerror(errno);
				return false;
			}
			Sent += static_cast<std::size_t>(Wrote);
		}
		return true;
	}

	struct FHttpResponse
	{
		int Status = 0;
		std::string Body;
	};

	/// One HTTP/1.1 request on its own connection, closed after the response.
	///
	/// `Connection: close` rather than keep-alive so the body's end is the
	/// socket's end and no chunked decoder is needed. The control endpoints are
	/// called a handful of times per run; the extra handshake is not worth a
	/// second parser.
	inline bool HttpRequest(
		const FUrl& Base,
		const std::string& Method,
		const std::string& Path,
		const std::string& ContentType,
		const std::string& Body,
		FHttpResponse& Out,
		std::string& Error)
	{
		const int Descriptor = DialTcp(Base.Host, Base.Port, Error);
		if (Descriptor < 0) return false;

		std::string Request = Method + " " + Path + " HTTP/1.1\r\n";
		Request += "Host: " + Base.Host + ":" + Base.Port + "\r\n";
		Request += "Connection: close\r\n";
		if (!Body.empty())
		{
			Request += "Content-Type: " + ContentType + "\r\n";
			Request += "Content-Length: " + std::to_string(Body.size()) + "\r\n";
		}
		Request += "\r\n";
		Request += Body;

		if (!WriteAll(Descriptor, Request, Error))
		{
			close(Descriptor);
			return false;
		}

		std::string Raw;
		char Buffer[4096];
		while (true)
		{
			const ssize_t Read = recv(Descriptor, Buffer, sizeof(Buffer), 0);
			if (Read < 0)
			{
				if (errno == EINTR) continue;
				Error = std::string("recv failed: ") + std::strerror(errno);
				close(Descriptor);
				return false;
			}
			if (Read == 0) break;
			Raw.append(Buffer, static_cast<std::size_t>(Read));
		}
		close(Descriptor);

		const std::size_t HeaderEnd = Raw.find("\r\n\r\n");
		if (HeaderEnd == std::string::npos)
		{
			Error = "no header terminator in the response";
			return false;
		}
		const std::size_t StatusStart = Raw.find(' ');
		if (StatusStart == std::string::npos)
		{
			Error = "malformed status line";
			return false;
		}
		Out.Status = std::atoi(Raw.c_str() + StatusStart + 1);
		Out.Body = Raw.substr(HeaderEnd + 4);
		return true;
	}

	/// A text-frame WebSocket client.
	///
	/// Non-blocking reads through `poll` because `expectSilence` needs the
	/// runner to keep receiving while asserting nothing arrives. A blocking read
	/// with a timeout would pass that verb by not looking, which is the failure
	/// it exists to catch — the same constraint the corpus README records for
	/// every other runner.
	class FWebSocket
	{
	public:
		FWebSocket() = default;
		FWebSocket(const FWebSocket&) = delete;
		FWebSocket& operator=(const FWebSocket&) = delete;

		~FWebSocket() { Close(); }

		bool IsOpen() const { return Descriptor >= 0; }

		bool Connect(const std::string& Url, std::string& Error)
		{
			FUrl Parsed;
			if (!ParseUrl(Url, Parsed, Error)) return false;

			Descriptor = DialTcp(Parsed.Host, Parsed.Port, Error);
			if (Descriptor < 0) return false;

			std::random_device Entropy;
			std::string Nonce(16, '\0');
			for (char& Byte : Nonce) Byte = static_cast<char>(Entropy() & 0xFF);
			const std::string Key = Base64(Nonce);

			std::string Request = "GET " + Parsed.Path + " HTTP/1.1\r\n";
			Request += "Host: " + Parsed.Host + ":" + Parsed.Port + "\r\n";
			Request += "Upgrade: websocket\r\n";
			Request += "Connection: Upgrade\r\n";
			Request += "Sec-WebSocket-Key: " + Key + "\r\n";
			Request += "Sec-WebSocket-Version: 13\r\n\r\n";
			if (!WriteAll(Descriptor, Request, Error))
			{
				Close();
				return false;
			}

			std::string Head;
			while (Head.find("\r\n\r\n") == std::string::npos)
			{
				char Buffer[1024];
				const ssize_t Read = recv(Descriptor, Buffer, sizeof(Buffer), 0);
				if (Read <= 0)
				{
					if (Read < 0 && errno == EINTR) continue;
					Error = "the upgrade response ended early";
					Close();
					return false;
				}
				Head.append(Buffer, static_cast<std::size_t>(Read));
			}

			const std::size_t HeaderEnd = Head.find("\r\n\r\n");
			// Anything after the terminator is already frame bytes.
			Inbound = Head.substr(HeaderEnd + 4);
			const std::string Header = Head.substr(0, HeaderEnd);

			if (Header.compare(0, 12, "HTTP/1.1 101") != 0)
			{
				Error = "the server refused the upgrade: " + Header.substr(0, Header.find("\r\n"));
				Close();
				return false;
			}

			const std::string Expected =
				Base64(Sha1(Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
			if (!HeaderHas(Header, "sec-websocket-accept:", Expected))
			{
				Error = "Sec-WebSocket-Accept did not match the key this client sent";
				Close();
				return false;
			}

			return true;
		}

		bool SendText(const std::string& Payload, std::string& Error)
		{
			if (Descriptor < 0)
			{
				Error = "send on a closed socket";
				return false;
			}

			std::string Frame;
			Frame.push_back(static_cast<char>(0x81)); // FIN + text

			const std::size_t Length = Payload.size();
			if (Length < 126)
			{
				Frame.push_back(static_cast<char>(0x80 | Length));
			}
			else if (Length <= 0xFFFF)
			{
				Frame.push_back(static_cast<char>(0x80 | 126));
				Frame.push_back(static_cast<char>((Length >> 8) & 0xFF));
				Frame.push_back(static_cast<char>(Length & 0xFF));
			}
			else
			{
				Frame.push_back(static_cast<char>(0x80 | 127));
				for (int Index = 7; Index >= 0; --Index)
				{
					Frame.push_back(static_cast<char>((Length >> (Index * 8)) & 0xFF));
				}
			}

			// A client frame must be masked; a server that receives an unmasked
			// one is required to fail the connection.
			std::random_device Entropy;
			unsigned char Mask[4];
			for (unsigned char& Byte : Mask) Byte = static_cast<unsigned char>(Entropy() & 0xFF);
			for (const unsigned char Byte : Mask) Frame.push_back(static_cast<char>(Byte));
			for (std::size_t Index = 0; Index < Length; ++Index)
			{
				Frame.push_back(static_cast<char>(
					static_cast<unsigned char>(Payload[Index]) ^ Mask[Index % 4]));
			}

			return WriteAll(Descriptor, Frame, Error);
		}

		/// Pump for `TimeoutMs`, appending every complete text message.
		///
		/// Returns false only on a transport error. A clean server close is
		/// normal — the fixture closes when a scenario ends — so it sets
		/// `bClosedByPeer` and the caller decides whether that mattered.
		bool Pump(int TimeoutMs, std::vector<std::string>& OutMessages, std::string& Error)
		{
			if (Descriptor < 0) return true;

			pollfd Watch{};
			Watch.fd = Descriptor;
			Watch.events = POLLIN;

			const int Ready = poll(&Watch, 1, TimeoutMs);
			if (Ready < 0)
			{
				if (errno == EINTR) return true;
				Error = std::string("poll failed: ") + std::strerror(errno);
				return false;
			}
			if (Ready > 0)
			{
				char Buffer[8192];
				const ssize_t Read = recv(Descriptor, Buffer, sizeof(Buffer), 0);
				if (Read < 0 && errno != EINTR)
				{
					Error = std::string("recv failed: ") + std::strerror(errno);
					return false;
				}
				if (Read == 0)
				{
					bClosedByPeer = true;
					Close();
					return true;
				}
				if (Read > 0) Inbound.append(Buffer, static_cast<std::size_t>(Read));
			}

			return Drain(OutMessages, Error);
		}

		bool ClosedByPeer() const { return bClosedByPeer; }

		void Close()
		{
			if (Descriptor >= 0)
			{
				close(Descriptor);
				Descriptor = -1;
			}
		}

	private:
		static bool HeaderHas(const std::string& Header, const std::string& LowerName, const std::string& Value)
		{
			std::string Lowered;
			Lowered.reserve(Header.size());
			for (const char Char : Header)
			{
				Lowered.push_back(static_cast<char>(
					Char >= 'A' && Char <= 'Z' ? Char - 'A' + 'a' : Char));
			}
			const std::size_t At = Lowered.find(LowerName);
			if (At == std::string::npos) return false;
			const std::size_t LineEnd = Header.find("\r\n", At);
			const std::string Line = Header.substr(
				At + LowerName.size(),
				LineEnd == std::string::npos ? std::string::npos : LineEnd - At - LowerName.size());
			std::size_t Start = Line.find_first_not_of(" \t");
			if (Start == std::string::npos) return false;
			std::size_t End = Line.find_last_not_of(" \t\r\n");
			return Line.substr(Start, End - Start + 1) == Value;
		}

		bool Drain(std::vector<std::string>& OutMessages, std::string& Error)
		{
			while (true)
			{
				if (Inbound.size() < 2) return true;

				const unsigned char First = static_cast<unsigned char>(Inbound[0]);
				const unsigned char Second = static_cast<unsigned char>(Inbound[1]);
				const bool bFinal = (First & 0x80) != 0;
				const unsigned char Opcode = First & 0x0F;
				const bool bMasked = (Second & 0x80) != 0;
				std::uint64_t Length = Second & 0x7F;
				std::size_t Cursor = 2;

				if (Length == 126)
				{
					if (Inbound.size() < Cursor + 2) return true;
					Length = (static_cast<std::uint64_t>(static_cast<unsigned char>(Inbound[Cursor])) << 8)
						| static_cast<unsigned char>(Inbound[Cursor + 1]);
					Cursor += 2;
				}
				else if (Length == 127)
				{
					if (Inbound.size() < Cursor + 8) return true;
					Length = 0;
					for (int Index = 0; Index < 8; ++Index)
					{
						Length = (Length << 8) | static_cast<unsigned char>(Inbound[Cursor + Index]);
					}
					Cursor += 8;
				}

				unsigned char Mask[4] = {0, 0, 0, 0};
				if (bMasked)
				{
					if (Inbound.size() < Cursor + 4) return true;
					for (int Index = 0; Index < 4; ++Index)
					{
						Mask[Index] = static_cast<unsigned char>(Inbound[Cursor + Index]);
					}
					Cursor += 4;
				}

				if (Inbound.size() < Cursor + Length) return true;

				std::string Payload = Inbound.substr(Cursor, static_cast<std::size_t>(Length));
				if (bMasked)
				{
					for (std::size_t Index = 0; Index < Payload.size(); ++Index)
					{
						Payload[Index] = static_cast<char>(
							static_cast<unsigned char>(Payload[Index]) ^ Mask[Index % 4]);
					}
				}
				Inbound.erase(0, Cursor + static_cast<std::size_t>(Length));

				switch (Opcode)
				{
				case 0x0: // continuation
					Partial += Payload;
					if (bFinal)
					{
						OutMessages.push_back(Partial);
						Partial.clear();
					}
					break;
				case 0x1: // text
					if (bFinal)
					{
						OutMessages.push_back(Payload);
					}
					else
					{
						Partial = Payload;
					}
					break;
				case 0x8: // close
					bClosedByPeer = true;
					Close();
					return true;
				case 0x9: // ping
				{
					std::string Pong;
					Pong.push_back(static_cast<char>(0x8A));
					Pong.push_back(static_cast<char>(0x80 | Payload.size()));
					std::random_device Entropy;
					unsigned char PongMask[4];
					for (unsigned char& Byte : PongMask) Byte = static_cast<unsigned char>(Entropy() & 0xFF);
					for (const unsigned char Byte : PongMask) Pong.push_back(static_cast<char>(Byte));
					for (std::size_t Index = 0; Index < Payload.size(); ++Index)
					{
						Pong.push_back(static_cast<char>(
							static_cast<unsigned char>(Payload[Index]) ^ PongMask[Index % 4]));
					}
					if (!WriteAll(Descriptor, Pong, Error)) return false;
					break;
				}
				case 0xA: // pong
					break;
				default:
					// Not ignored. A binary frame here would mean the fixture
					// changed shape, and dropping it would read as silence.
					Error = "unexpected websocket opcode " + std::to_string(Opcode);
					return false;
				}
			}
		}

		int Descriptor = -1;
		std::string Inbound;
		std::string Partial;
		bool bClosedByPeer = false;
	};
}
