// The envelope, in plain C++17 with no engine types.
//
// That is the one deliberate decision in this file and it is a reaction to F20.
// The WebGL transport in the Unity binding was the only file in this repository
// that nothing compiled — written, reviewed, and never once put through a
// compiler, because it needed Unity to build. An Unreal module written the
// idiomatic way, in FString and TSharedPtr<FJsonObject>, would be that file
// again and larger: unbuildable without the engine, and therefore unchecked
// until someone opens it in an editor nobody here has.
//
// So the protocol core takes no engine dependency and is compiled and exercised
// by `sdks/cpp/verify.sh` under an ordinary compiler. It is the C++ SDK, which the
// Unreal plugin vendors; the engine glue — subsystem, IWebSocket, Blueprint
// nodes — stays in the plugin where it belongs and is explicitly NOT verified
// here.
//
// The cost is real and worth stating rather than hiding: `std::optional` and
// `std::string` are unidiomatic in an Unreal module, and every value crossing
// into the engine layer pays a conversion. What that buys is the classification
// logic — the part with the bug in it — being checkable at all.

#pragma once

#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace Haiven
{
	inline constexpr const char* AuthType = "auth";
	inline constexpr const char* AuthSuccessType = "auth.success";
	inline constexpr const char* AuthExpiredType = "auth.expired";
	inline constexpr const char* AuthFailureType = "auth.failure";
	inline constexpr const char* AuthExpiredDroppedType = "auth.expired.dropped";

	/// The correlated half of an `auth.expired.dropped` report
	/// (`schemas/ws-dropped-frames`): the replies the server discarded while the
	/// session was expired. When `bTruncated` is set the list is a sample, and
	/// every request still unanswered has to be treated as lost.
	struct FDroppedFrames
	{
		std::vector<std::string> RequestIds;
		bool bTruncated = false;
	};

	/// One frame the server sent, after parsing and before interpretation.
	///
	/// `Success` is `std::optional<bool>` and that is the whole design. The two
	/// outbound shapes are told apart by the PRESENCE of `success`, not its
	/// value (F10): a reply always carries it, an event never does. A plain
	/// `bool` cannot hold "absent", so it maps an event to `false` and turns
	/// every one of them into a *failed reply* — which is precisely what the C#
	/// binding hit with a non-nullable `bool`, and what GDScript hit with a
	/// falsy `null`.
	///
	/// Three bindings, three languages, the same collapse available and
	/// idiomatic in each. It is a property of the discriminant rather than of
	/// any language, so this one is written to make the mistake impossible
	/// rather than merely discouraged — see the static_assert below.
	struct FServerFrame
	{
		std::string Type;
		std::optional<std::string> RequestId;
		std::optional<bool> Success;
		std::optional<std::string> Error;
		bool bHasPayload = false;
		/// Set by the parser for `auth.expired.dropped`, the one frame whose payload
		/// the session core itself acts on.
		std::optional<FDroppedFrames> Dropped;
	};

	// The finding, encoded so it cannot be quietly undone. Narrowing Success to
	// `bool` for tidiness is exactly the change that reintroduces the collapse,
	// and it would otherwise compile, pass a casual review, and fail only
	// against a live server.
	static_assert(
		!std::is_same_v<decltype(FServerFrame::Success), bool>,
		"Success must be able to hold absent: an event omits the field, and a bool maps that to false, "
		"which classifies every event as a failed reply. See haiven-sdk/docs/findings.md F10.");

	/// Whether a frame answers something this client sent.
	///
	/// Presence, never value. A refused reply is still a reply.
	inline bool IsReply(const FServerFrame& Frame)
	{
		return Frame.Success.has_value();
	}

	/// Whether a reply reports success. Meaningless for an event; ask IsReply first.
	inline bool Succeeded(const FServerFrame& Frame)
	{
		return Frame.Success.value_or(false);
	}

	/// Whether the server initiated this reply rather than answering a request.
	///
	/// Absent and empty are different states and both occur: a server-initiated
	/// reply sends `requestId` as the empty string — present, empty — while an
	/// event omits the field entirely. A binding that treats them alike loses
	/// the distinction that says which pump wrote the frame.
	inline bool IsServerInitiatedReply(const FServerFrame& Frame)
	{
		return IsReply(Frame) && Frame.RequestId.has_value() && Frame.RequestId->empty();
	}
}
