// Server frames in, client frames out: the wire half of the session, as text.
//
// `FSessionCore` deals in parsed frames and leaves JSON to its host. Every host
// used to bring its own: the Unreal plugin through `FJsonObject`, the two C++
// conformance runners through a restatement of the same rules over `FJson`.
// This is that restatement promoted, so the plugin, the runners and a plain C++
// host (`HaivenClient.h`) parse and write the wire with one piece of code and
// cannot disagree about what a frame says.
#pragma once

#include "HaivenJson.h"
#include "HaivenProtocol.h"
#include "HaivenSessionCore.h"

#include <string>
#include <utility>

namespace Haiven
{
	/// One parsed server frame. Presence of `success` decides reply versus
	/// event, `requestId` is optional and its empty string is meaningful, and
	/// `payload` is observed only for presence except on `auth.expired.dropped`,
	/// whose report the session core acts on.
	inline bool ParseServerFrame(const FJson& Object, Haiven::FServerFrame& OutFrame)
	{
		if (!Object.IsObject()) return false;
		const FJson* Type = Object.Find("type");
		if (Type == nullptr || Type->Kind != FJson::EKind::String) return false;
		OutFrame.Type = Type->String;

		if (const FJson* RequestId = Object.Find("requestId");
			RequestId != nullptr && RequestId->Kind == FJson::EKind::String)
		{
			OutFrame.RequestId = RequestId->String;
		}
		if (const FJson* Success = Object.Find("success");
			Success != nullptr && Success->Kind == FJson::EKind::Bool)
		{
			OutFrame.Success = Success->Bool;
		}
		if (const FJson* Error = Object.Find("error");
			Error != nullptr && Error->Kind == FJson::EKind::String)
		{
			OutFrame.Error = Error->String;
		}
		OutFrame.bHasPayload = Object.Has("payload");
		if (const FJson* Report = Object.Find("payload");
			OutFrame.Type == Haiven::AuthExpiredDroppedType && Report != nullptr && Report->IsObject())
		{
			Haiven::FDroppedFrames Dropped;
			if (const FJson* Ids = Report->Find("requestIds"); Ids != nullptr && Ids->Kind == FJson::EKind::Array)
			{
				for (const FJson& Id : Ids->Items)
				{
					if (Id.IsString()) Dropped.RequestIds.push_back(Id.String);
				}
			}
			const FJson* Truncated = Report->Find("truncated");
			Dropped.bTruncated = Truncated != nullptr && Truncated->Kind == FJson::EKind::Bool && Truncated->Bool;
			OutFrame.Dropped = std::move(Dropped);
		}
		return true;
	}

	/// One server frame from the text a socket delivered; false when it is not
	/// a JSON object with a string `type`, which a host drops rather than acting on.
	inline bool ParseServerFrame(const std::string& Text, FServerFrame& OutFrame)
	{
		FJson Object;
		std::string Error;
		return FJson::Parse(Text, Object, Error) && ParseServerFrame(Object, OutFrame);
	}

	/// The text a socket sends for one client frame.
	///
	/// `auth`'s payload is the bearer itself and is written as a JSON string.
	/// Every other payload is JSON text, checked to parse and then written
	/// verbatim: re-encoding it through `FJson` would round its numbers through
	/// `double`, and an id above 2^53 would arrive as a different id. Returns
	/// false, writing nothing, when that payload is not JSON.
	inline bool SerializeClientFrame(const FClientFrame& Frame, std::string& Out)
	{
		std::string Text = "{\"type\":";
		JsonDetail::WriteEscaped(Frame.Type, Text);
		Text += ",\"requestId\":";
		JsonDetail::WriteEscaped(Frame.RequestId, Text);
		if (Frame.PayloadJson.has_value())
		{
			Text += ",\"payload\":";
			if (Frame.Type == AuthType)
			{
				JsonDetail::WriteEscaped(*Frame.PayloadJson, Text);
			}
			else
			{
				FJson Checked;
				std::string Error;
				if (!FJson::Parse(*Frame.PayloadJson, Checked, Error)) return false;
				Text += *Frame.PayloadJson;
			}
		}
		Text += "}";
		Out = std::move(Text);
		return true;
	}
}
