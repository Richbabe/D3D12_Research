#pragma once

#include <cstring>
#include <cstdlib>
#include <type_traits>

#include "Core/Stream.h"

/*
	Minimal read-only JSON reader.

	Deviates from the spec in two ways so it can read hand-authored data: trailing commas are
	accepted and '//' line comments are skipped. Lookups of missing keys or out-of-range indices
	return a null value instead of failing, so a chain of accessors never needs intermediate
	checks and every getter takes the default to use when the field isn't there.
*/
namespace Json
{
	class Value
	{
	public:
		enum class Type
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object,
		};

		Type GetType() const	{ return m_Type; }
		bool IsNull() const		{ return m_Type == Type::Null; }

		bool		GetBool(bool defaultValue = false) const			{ return m_Type == Type::Bool ? m_Bool : defaultValue; }
		float		GetFloat(float defaultValue = 0.0f) const		{ return m_Type == Type::Number ? (float)m_Number : defaultValue; }
		int			GetInt(int defaultValue = 0) const				{ return m_Type == Type::Number ? (int)m_Number : defaultValue; }
		const char* GetString(const char* pDefault = "") const		{ return m_Type == Type::String ? m_String.c_str() : pDefault; }

		// Element count of an array, or member count of an object. 0 for anything else.
		uint32 GetSize() const { return (uint32)m_Elements.size(); }

		// Constrained to integers so that a literal 0 resolves here instead of being ambiguous with
		// the member lookup below, which it would also match as a null pointer constant.
		template<typename IndexType, typename = std::enable_if_t<std::is_integral_v<IndexType>>>
		const Value& operator[](IndexType index) const
		{
			return (size_t)index < m_Elements.size() ? m_Elements[(size_t)index] : GetNull();
		}

		// Name of the member at the given index, empty unless this is an object.
		const char* GetName(uint32 index) const
		{
			return index < m_Names.size() ? m_Names[index].c_str() : "";
		}

		const Value& operator[](const char* pName) const
		{
			if (m_Type == Type::Object)
			{
				for (size_t i = 0; i < m_Names.size(); ++i)
				{
					if (m_Names[i] == pName)
						return m_Elements[i];
				}
			}
			return GetNull();
		}

		static const Value& GetNull()
		{
			static const Value nullValue;
			return nullValue;
		}

	private:
		friend struct Parser;

		Type m_Type = Type::Null;
		bool m_Bool = false;
		double m_Number = 0;
		String m_String;
		// Member names of an object, parallel to m_Elements. Empty for arrays.
		Array<String> m_Names;
		Array<Value> m_Elements;
	};

	struct Parser
	{
		const char* pCursor = nullptr;

		void SkipIgnored()
		{
			for (;;)
			{
				while (*pCursor && (unsigned char)*pCursor <= ' ')
					++pCursor;

				if (pCursor[0] == '/' && pCursor[1] == '/')
				{
					while (*pCursor && *pCursor != '\n')
						++pCursor;
					continue;
				}
				return;
			}
		}

		bool Consume(char c)
		{
			SkipIgnored();
			if (*pCursor != c)
				return false;
			++pCursor;
			return true;
		}

		bool ParseString(String& out)
		{
			if (!Consume('"'))
				return false;

			out.clear();
			while (*pCursor && *pCursor != '"')
			{
				if (*pCursor != '\\')
				{
					out += *pCursor++;
					continue;
				}

				++pCursor;
				switch (*pCursor)
				{
				case '"':	out += '"';		break;
				case '\\':	out += '\\';	break;
				case '/':	out += '/';		break;
				case 'b':	out += '\b';	break;
				case 'f':	out += '\f';	break;
				case 'n':	out += '\n';	break;
				case 'r':	out += '\r';	break;
				case 't':	out += '\t';	break;
				case 'u':
				{
					uint32 codepoint = 0;
					for (int i = 0; i < 4; ++i)
					{
						char digit = pCursor[1 + i];
						if (digit >= '0' && digit <= '9')		codepoint = codepoint * 16 + (digit - '0');
						else if (digit >= 'a' && digit <= 'f')	codepoint = codepoint * 16 + (digit - 'a' + 10);
						else if (digit >= 'A' && digit <= 'F')	codepoint = codepoint * 16 + (digit - 'A' + 10);
						else return false;
					}
					pCursor += 4;
					// Surrogate pairs aren't resolved, they end up as two replacement sequences.
					if (codepoint < 0x80)
					{
						out += (char)codepoint;
					}
					else if (codepoint < 0x800)
					{
						out += (char)(0xC0 | (codepoint >> 6));
						out += (char)(0x80 | (codepoint & 0x3F));
					}
					else
					{
						out += (char)(0xE0 | (codepoint >> 12));
						out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
						out += (char)(0x80 | (codepoint & 0x3F));
					}
					break;
				}
				default:
					return false;
				}
				++pCursor;
			}

			return Consume('"');
		}

		bool ParseValue(Value& out)
		{
			SkipIgnored();

			switch (*pCursor)
			{
			case '\0':
				return false;
			case '{':
			{
				++pCursor;
				out.m_Type = Value::Type::Object;
				while (!Consume('}'))
				{
					String& name = out.m_Names.emplace_back();
					if (!ParseString(name))
						return false;
					if (!Consume(':'))
						return false;
					if (!ParseValue(out.m_Elements.emplace_back()))
						return false;

					SkipIgnored();
					if (*pCursor == ',')
						++pCursor;
					else if (*pCursor != '}')
						return false;
				}
				return true;
			}
			case '[':
			{
				++pCursor;
				out.m_Type = Value::Type::Array;
				while (!Consume(']'))
				{
					if (!ParseValue(out.m_Elements.emplace_back()))
						return false;

					SkipIgnored();
					if (*pCursor == ',')
						++pCursor;
					else if (*pCursor != ']')
						return false;
				}
				return true;
			}
			case '"':
				out.m_Type = Value::Type::String;
				return ParseString(out.m_String);
			case 't':
				if (strncmp(pCursor, "true", 4) != 0)
					return false;
				pCursor += 4;
				out.m_Type = Value::Type::Bool;
				out.m_Bool = true;
				return true;
			case 'f':
				if (strncmp(pCursor, "false", 5) != 0)
					return false;
				pCursor += 5;
				out.m_Type = Value::Type::Bool;
				out.m_Bool = false;
				return true;
			case 'n':
				if (strncmp(pCursor, "null", 4) != 0)
					return false;
				pCursor += 4;
				return true;
			default:
			{
				char* pEnd = nullptr;
				double number = strtod(pCursor, &pEnd);
				if (pEnd == pCursor)
					return false;
				pCursor = pEnd;
				out.m_Type = Value::Type::Number;
				out.m_Number = number;
				return true;
			}
			}
		}
	};

	inline bool Parse(const char* pText, Value& out)
	{
		Parser parser{ pText };
		return parser.ParseValue(out);
	}

	inline bool ParseFile(const char* pFilePath, Value& out)
	{
		FileStream file;
		if (!file.Open(pFilePath, FileMode::Read))
			return false;

		String text(file.GetLength(), '\0');
		if (!file.Read(text.data(), (uint32)text.size()))
			return false;

		return Parse(text.c_str(), out);
	}
}
