// A JSON reader/writer in plain C++17, for the conformance runner only.
//
// Every other runner gets this from its platform: node has JSON, C# has
// System.Text.Json, GDScript has JSON.parse_string. C++ has none, and under
// Unreal the answer is FJsonSerializer — an engine type, which is exactly what
// `HaivenProtocol.h` refuses to depend on and for the same reason (F20). A
// runner that needed the engine to parse the suite could not be compiled on a
// machine without one, which is every machine this repository is checked on
// today.
//
// So it is written here rather than pulled in: no third-party header appears in
// `verify.sh`'s command line, and the runner stays a `g++ -std=c++17` away from
// running. It is deliberately only as much JSON as the corpus and the wire use.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace HaivenConformance
{
	class FJson
	{
	public:
		enum class EKind
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object,
		};

		EKind Kind = EKind::Null;
		bool Bool = false;
		double Number = 0.0;
		std::string String;
		std::vector<FJson> Items;

		/// Ordered so a round trip preserves document order. The corpus is small
		/// and read once; a map would buy nothing and lose the ordering that
		/// makes a serialized frame comparable by eye in a failure message.
		std::vector<std::pair<std::string, FJson>> Members;

		static FJson MakeString(std::string Value)
		{
			FJson Result;
			Result.Kind = EKind::String;
			Result.String = std::move(Value);
			return Result;
		}

		static FJson MakeObject()
		{
			FJson Result;
			Result.Kind = EKind::Object;
			return Result;
		}

		bool IsObject() const { return Kind == EKind::Object; }
		bool IsString() const { return Kind == EKind::String; }

		const FJson* Find(const std::string& Key) const
		{
			if (Kind != EKind::Object) return nullptr;
			for (const auto& Member : Members)
			{
				if (Member.first == Key) return &Member.second;
			}
			return nullptr;
		}

		bool Has(const std::string& Key) const { return Find(Key) != nullptr; }

		void Set(std::string Key, FJson Value)
		{
			Kind = EKind::Object;
			for (auto& Member : Members)
			{
				if (Member.first == Key)
				{
					Member.second = std::move(Value);
					return;
				}
			}
			Members.emplace_back(std::move(Key), std::move(Value));
		}

		std::string StringOr(const std::string& Key, const std::string& Fallback = std::string()) const
		{
			const FJson* Found = Find(Key);
			return (Found != nullptr && Found->Kind == EKind::String) ? Found->String : Fallback;
		}

		double NumberOr(const std::string& Key, double Fallback) const
		{
			const FJson* Found = Find(Key);
			return (Found != nullptr && Found->Kind == EKind::Number) ? Found->Number : Fallback;
		}

		std::vector<std::string> Keys() const
		{
			std::vector<std::string> Result;
			Result.reserve(Members.size());
			for (const auto& Member : Members) Result.push_back(Member.first);
			return Result;
		}

		/// Structural equality, used to compare a corpus `fields` value against
		/// what actually arrived. Numbers compare exactly on purpose: the corpus
		/// carries small integers, and a tolerance here would quietly accept a
		/// server that started answering a different one.
		bool Equals(const FJson& Other) const
		{
			if (Kind != Other.Kind) return false;
			switch (Kind)
			{
			case EKind::Null:
				return true;
			case EKind::Bool:
				return Bool == Other.Bool;
			case EKind::Number:
				return Number == Other.Number;
			case EKind::String:
				return String == Other.String;
			case EKind::Array:
			{
				if (Items.size() != Other.Items.size()) return false;
				for (std::size_t Index = 0; Index < Items.size(); ++Index)
				{
					if (!Items[Index].Equals(Other.Items[Index])) return false;
				}
				return true;
			}
			case EKind::Object:
			{
				if (Members.size() != Other.Members.size()) return false;
				for (const auto& Member : Members)
				{
					const FJson* Mirror = Other.Find(Member.first);
					if (Mirror == nullptr || !Member.second.Equals(*Mirror)) return false;
				}
				return true;
			}
			}
			return false;
		}

		std::string Dump() const
		{
			std::string Out;
			Write(Out);
			return Out;
		}

		static bool Parse(const std::string& Text, FJson& Out, std::string& Error);

	private:
		void Write(std::string& Out) const;
	};
}

namespace HaivenConformance
{
	namespace JsonDetail
	{
		struct FReader
		{
			const std::string& Text;
			std::size_t At = 0;
			std::string Error;

			explicit FReader(const std::string& InText) : Text(InText) {}

			void SkipSpace()
			{
				while (At < Text.size())
				{
					const char Char = Text[At];
					if (Char == ' ' || Char == '\t' || Char == '\n' || Char == '\r') ++At;
					else break;
				}
			}

			bool Fail(const char* What)
			{
				if (Error.empty()) Error = std::string(What) + " at offset " + std::to_string(At);
				return false;
			}

			/// UTF-8 for one code point. `\u` escapes arrive as UTF-16, so a
			/// surrogate pair has to be recombined before encoding — the suite is
			/// ASCII today, but a `why` field with a dash in it would otherwise
			/// become two replacement characters and the failure would read as a
			/// corpus problem.
			static void AppendUtf8(std::string& Out, std::uint32_t Code)
			{
				if (Code < 0x80)
				{
					Out.push_back(static_cast<char>(Code));
				}
				else if (Code < 0x800)
				{
					Out.push_back(static_cast<char>(0xC0 | (Code >> 6)));
					Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
				}
				else if (Code < 0x10000)
				{
					Out.push_back(static_cast<char>(0xE0 | (Code >> 12)));
					Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
					Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
				}
				else
				{
					Out.push_back(static_cast<char>(0xF0 | (Code >> 18)));
					Out.push_back(static_cast<char>(0x80 | ((Code >> 12) & 0x3F)));
					Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
					Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
				}
			}

			bool ReadHex4(std::uint32_t& Out)
			{
				if (At + 4 > Text.size()) return Fail("truncated \\u escape");
				Out = 0;
				for (int Index = 0; Index < 4; ++Index)
				{
					const char Char = Text[At++];
					Out <<= 4;
					if (Char >= '0' && Char <= '9') Out |= static_cast<std::uint32_t>(Char - '0');
					else if (Char >= 'a' && Char <= 'f') Out |= static_cast<std::uint32_t>(Char - 'a' + 10);
					else if (Char >= 'A' && Char <= 'F') Out |= static_cast<std::uint32_t>(Char - 'A' + 10);
					else return Fail("bad hex digit in \\u escape");
				}
				return true;
			}

			bool ReadString(std::string& Out)
			{
				if (At >= Text.size() || Text[At] != '"') return Fail("expected a string");
				++At;
				Out.clear();
				while (At < Text.size())
				{
					const char Char = Text[At++];
					if (Char == '"') return true;
					if (Char != '\\')
					{
						Out.push_back(Char);
						continue;
					}
					if (At >= Text.size()) return Fail("truncated escape");
					const char Escape = Text[At++];
					switch (Escape)
					{
					case '"': Out.push_back('"'); break;
					case '\\': Out.push_back('\\'); break;
					case '/': Out.push_back('/'); break;
					case 'b': Out.push_back('\b'); break;
					case 'f': Out.push_back('\f'); break;
					case 'n': Out.push_back('\n'); break;
					case 'r': Out.push_back('\r'); break;
					case 't': Out.push_back('\t'); break;
					case 'u':
					{
						std::uint32_t Code = 0;
						if (!ReadHex4(Code)) return false;
						if (Code >= 0xD800 && Code <= 0xDBFF && At + 6 <= Text.size()
							&& Text[At] == '\\' && Text[At + 1] == 'u')
						{
							At += 2;
							std::uint32_t Low = 0;
							if (!ReadHex4(Low)) return false;
							if (Low >= 0xDC00 && Low <= 0xDFFF)
							{
								Code = 0x10000 + ((Code - 0xD800) << 10) + (Low - 0xDC00);
							}
							else
							{
								AppendUtf8(Out, Code);
								Code = Low;
							}
						}
						AppendUtf8(Out, Code);
						break;
					}
					default:
						return Fail("unknown escape");
					}
				}
				return Fail("unterminated string");
			}

			bool ReadValue(FJson& Out)
			{
				SkipSpace();
				if (At >= Text.size()) return Fail("unexpected end of input");
				const char Char = Text[At];

				if (Char == '"')
				{
					Out.Kind = FJson::EKind::String;
					return ReadString(Out.String);
				}
				if (Char == '{') return ReadObject(Out);
				if (Char == '[') return ReadArray(Out);
				if (Text.compare(At, 4, "true") == 0)
				{
					At += 4;
					Out.Kind = FJson::EKind::Bool;
					Out.Bool = true;
					return true;
				}
				if (Text.compare(At, 5, "false") == 0)
				{
					At += 5;
					Out.Kind = FJson::EKind::Bool;
					Out.Bool = false;
					return true;
				}
				if (Text.compare(At, 4, "null") == 0)
				{
					At += 4;
					Out.Kind = FJson::EKind::Null;
					return true;
				}
				return ReadNumber(Out);
			}

			bool ReadNumber(FJson& Out)
			{
				const std::size_t Start = At;
				if (At < Text.size() && (Text[At] == '-' || Text[At] == '+')) ++At;
				bool bAnyDigits = false;
				while (At < Text.size() && Text[At] >= '0' && Text[At] <= '9')
				{
					++At;
					bAnyDigits = true;
				}
				if (At < Text.size() && Text[At] == '.')
				{
					++At;
					while (At < Text.size() && Text[At] >= '0' && Text[At] <= '9')
					{
						++At;
						bAnyDigits = true;
					}
				}
				if (bAnyDigits && At < Text.size() && (Text[At] == 'e' || Text[At] == 'E'))
				{
					++At;
					if (At < Text.size() && (Text[At] == '-' || Text[At] == '+')) ++At;
					while (At < Text.size() && Text[At] >= '0' && Text[At] <= '9') ++At;
				}
				if (!bAnyDigits) return Fail("expected a value");
				Out.Kind = FJson::EKind::Number;
				Out.Number = std::stod(Text.substr(Start, At - Start));
				return true;
			}

			bool ReadArray(FJson& Out)
			{
				++At;
				Out.Kind = FJson::EKind::Array;
				SkipSpace();
				if (At < Text.size() && Text[At] == ']')
				{
					++At;
					return true;
				}
				while (true)
				{
					FJson Item;
					if (!ReadValue(Item)) return false;
					Out.Items.push_back(std::move(Item));
					SkipSpace();
					if (At < Text.size() && Text[At] == ',')
					{
						++At;
						continue;
					}
					if (At < Text.size() && Text[At] == ']')
					{
						++At;
						return true;
					}
					return Fail("expected , or ] in array");
				}
			}

			bool ReadObject(FJson& Out)
			{
				++At;
				Out.Kind = FJson::EKind::Object;
				SkipSpace();
				if (At < Text.size() && Text[At] == '}')
				{
					++At;
					return true;
				}
				while (true)
				{
					SkipSpace();
					std::string Key;
					if (!ReadString(Key)) return false;
					SkipSpace();
					if (At >= Text.size() || Text[At] != ':') return Fail("expected : after a key");
					++At;
					FJson Value;
					if (!ReadValue(Value)) return false;
					Out.Members.emplace_back(std::move(Key), std::move(Value));
					SkipSpace();
					if (At < Text.size() && Text[At] == ',')
					{
						++At;
						continue;
					}
					if (At < Text.size() && Text[At] == '}')
					{
						++At;
						return true;
					}
					return Fail("expected , or } in object");
				}
			}
		};

		inline void WriteEscaped(const std::string& Value, std::string& Out)
		{
			Out.push_back('"');
			for (const char Char : Value)
			{
				switch (Char)
				{
				case '"': Out += "\\\""; break;
				case '\\': Out += "\\\\"; break;
				case '\b': Out += "\\b"; break;
				case '\f': Out += "\\f"; break;
				case '\n': Out += "\\n"; break;
				case '\r': Out += "\\r"; break;
				case '\t': Out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(Char) < 0x20)
					{
						static const char* Digits = "0123456789abcdef";
						Out += "\\u00";
						Out.push_back(Digits[(static_cast<unsigned char>(Char) >> 4) & 0xF]);
						Out.push_back(Digits[static_cast<unsigned char>(Char) & 0xF]);
					}
					else
					{
						Out.push_back(Char);
					}
				}
			}
			Out.push_back('"');
		}

		inline std::string WriteNumber(double Value)
		{
			if (Value == static_cast<double>(static_cast<long long>(Value)))
			{
				return std::to_string(static_cast<long long>(Value));
			}
			std::string Text = std::to_string(Value);
			while (Text.size() > 1 && Text.back() == '0') Text.pop_back();
			if (!Text.empty() && Text.back() == '.') Text.pop_back();
			return Text;
		}
	}

	inline bool FJson::Parse(const std::string& Text, FJson& Out, std::string& Error)
	{
		JsonDetail::FReader Reader(Text);
		if (!Reader.ReadValue(Out))
		{
			Error = Reader.Error;
			return false;
		}
		Reader.SkipSpace();
		if (Reader.At != Text.size())
		{
			Error = "trailing input at offset " + std::to_string(Reader.At);
			return false;
		}
		return true;
	}

	inline void FJson::Write(std::string& Out) const
	{
		switch (Kind)
		{
		case EKind::Null:
			Out += "null";
			return;
		case EKind::Bool:
			Out += Bool ? "true" : "false";
			return;
		case EKind::Number:
			Out += JsonDetail::WriteNumber(Number);
			return;
		case EKind::String:
			JsonDetail::WriteEscaped(String, Out);
			return;
		case EKind::Array:
		{
			Out.push_back('[');
			for (std::size_t Index = 0; Index < Items.size(); ++Index)
			{
				if (Index != 0) Out.push_back(',');
				Items[Index].Write(Out);
			}
			Out.push_back(']');
			return;
		}
		case EKind::Object:
		{
			Out.push_back('{');
			for (std::size_t Index = 0; Index < Members.size(); ++Index)
			{
				if (Index != 0) Out.push_back(',');
				JsonDetail::WriteEscaped(Members[Index].first, Out);
				Out.push_back(':');
				Members[Index].second.Write(Out);
			}
			Out.push_back('}');
			return;
		}
		}
	}
}
