/*
 *  AudioCast for open.mp and SA-MP
 *  Copyright (c) 2026 devbluen
 */

#include "text.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <memory>
#include <random>
#include <utility>
#include <utf8proc.h>

namespace Text
{
namespace
{
bool isIdChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

// A plain word such as "Imagination" is also 11 characters long, so a bare
// ID is only recognised when it does not look like a normal word.
bool looksLikeBareId(const std::string& value)
{
	if (!isVideoId(value)) return false;
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		const unsigned char c = static_cast<unsigned char>(value[index]);
		if (std::isdigit(c) || c == '-' || c == '_' || (index > 0 && std::isupper(c))) return true;
	}
	return false;
}

std::string firstSegment(const std::string& path)
{
	const std::size_t end = path.find_first_of("/?#&");
	return end == std::string::npos ? path : path.substr(0, end);
}

std::string queryParameter(const std::string& query, const std::string& name)
{
	std::size_t position = 0;
	while (position < query.size())
	{
		std::size_t end = query.find('&', position);
		if (end == std::string::npos) end = query.size();
		const std::string pair = query.substr(position, end - position);
		const std::size_t equals = pair.find('=');
		if (equals != std::string::npos && pair.substr(0, equals) == name) return pair.substr(equals + 1);
		position = end + 1;
	}
	return {};
}

Query parseUrl(const std::string& input)
{
	std::string rest = input;
	const std::size_t scheme = rest.find("://");
	if (scheme != std::string::npos)
	{
		const std::string name = lower(rest.substr(0, scheme));
		if (name != "http" && name != "https") return {};
		rest = rest.substr(scheme + 3);
	}

	const std::size_t slash = rest.find_first_of("/?#");
	std::string host = lower(slash == std::string::npos ? rest : rest.substr(0, slash));
	std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
	if (const std::size_t colon = host.find(':'); colon != std::string::npos) host.resize(colon);
	for (const char* prefix : { "www.", "m.", "music." })
	{
		const std::size_t length = std::char_traits<char>::length(prefix);
		if (host.compare(0, length, prefix) == 0) host = host.substr(length);
	}

	std::string id;
	if (host == "youtu.be")
	{
		if (!path.empty() && path[0] == '/') id = firstSegment(path.substr(1));
	}
	else if (host == "youtube.com" || host == "youtube-nocookie.com")
	{
		std::string query;
		if (const std::size_t mark = path.find('?'); mark != std::string::npos)
		{
			query = path.substr(mark + 1);
			if (const std::size_t hash = query.find('#'); hash != std::string::npos) query.resize(hash);
			path.resize(mark);
		}
		if (path == "/watch" || path == "/watch/")
		{
			id = queryParameter(query, "v");
		}
		else
		{
			for (const char* prefix : { "/shorts/", "/embed/", "/live/", "/v/", "/e/" })
			{
				const std::size_t length = std::char_traits<char>::length(prefix);
				if (path.compare(0, length, prefix) == 0)
				{
					id = firstSegment(path.substr(length));
					break;
				}
			}
		}
	}

	if (!isVideoId(id)) return {};
	return Query { Query::Kind::Video, id };
}
}

bool isVideoId(const std::string& value)
{
	return value.size() == 11 && std::all_of(value.begin(), value.end(), isIdChar);
}

Query parseQuery(const std::string& input)
{
	const std::string text = trim(input);
	if (text.empty()) return {};

	const std::string lowered = lower(text);
	bool looksLikeUrl = lowered.find("://") != std::string::npos;
	for (const char* prefix : { "www.", "youtu.be/", "youtube.com/", "m.youtube.com/", "music.youtube.com/" })
	{
		if (lowered.compare(0, std::char_traits<char>::length(prefix), prefix) == 0) looksLikeUrl = true;
	}
	if (looksLikeUrl) return parseUrl(text);

	if (looksLikeBareId(text)) return Query { Query::Kind::Video, text };

	// Free text: drop control characters and keep the search short.
	std::string terms;
	for (char c : text)
	{
		if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) terms.push_back(c);
	}
	if (terms.size() > 200) terms.resize(200);
	terms = trim(terms);
	if (terms.empty()) return {};
	return Query { Query::Kind::Search, terms };
}

std::string utf8ToLatin1(const std::string& text)
{
	std::string output;
	output.reserve(text.size());
	for (std::size_t index = 0; index < text.size();)
	{
		const unsigned char lead = static_cast<unsigned char>(text[index]);
		std::uint32_t codepoint = 0;
		std::size_t length = 0;
		if (lead < 0x80) { codepoint = lead; length = 1; }
		else if ((lead & 0xE0) == 0xC0) { codepoint = lead & 0x1F; length = 2; }
		else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0F; length = 3; }
		else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07; length = 4; }

		bool valid = length > 0 && index + length <= text.size();
		for (std::size_t offset = 1; valid && offset < length; ++offset)
		{
			const unsigned char next = static_cast<unsigned char>(text[index + offset]);
			if ((next & 0xC0) != 0x80) valid = false;
			else codepoint = (codepoint << 6) | (next & 0x3F);
		}
		if (!valid)
		{
			// Not UTF-8: keep the byte as it is.
			output.push_back(static_cast<char>(lead));
			++index;
			continue;
		}
		index += length;

		if (codepoint < 0x100) output.push_back(static_cast<char>(codepoint));
		else if (codepoint == 0x2018 || codepoint == 0x2019 || codepoint == 0x2032) output.push_back('\'');
		else if (codepoint == 0x201C || codepoint == 0x201D || codepoint == 0x2033) output.push_back('"');
		else if (codepoint == 0x2013 || codepoint == 0x2014 || codepoint == 0x2212) output.push_back('-');
		else if (codepoint == 0x2026) output += "...";
		else if (codepoint == 0x2022) output.push_back('*');
		else if (codepoint >= 0xFE00 && codepoint <= 0xFE0F) continue; // variation selectors
		else output.push_back('?');
	}
	return output;
}

std::string latin1ToUtf8(const std::string& text)
{
	std::string output;
	output.reserve(text.size());
	for (char c : text)
	{
		const unsigned char byte = static_cast<unsigned char>(c);
		if (byte < 0x80)
		{
			output.push_back(c);
		}
		else
		{
			output.push_back(static_cast<char>(0xC0 | (byte >> 6)));
			output.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
		}
	}
	return output;
}

namespace
{
// Windows code pages: the character of each byte from 0x80, 0 where none.
constexpr std::uint16_t kWindows1250[128] = {
	0x20AC, 0x0000, 0x201A, 0x0000, 0x201E, 0x2026, 0x2020, 0x2021,
	0x0000, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
	0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x0000, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
	0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
	0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
	0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
	0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
	0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
	0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
	0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
	0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
	0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
	0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
	0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
	0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

constexpr std::uint16_t kWindows1252[128] = {
	0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
	0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
	0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
	0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
	0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
	0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
	0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
	0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
	0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
	0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
	0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
	0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
	0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
	0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

// Letters that have no decomposition into a plain letter and an accent.
constexpr std::pair<utf8proc_int32_t, char> kUndecomposable[] = {
	{ 0x00C6, 'A' }, { 0x00D0, 'D' }, { 0x00D8, 'O' }, { 0x00DE, 'T' }, { 0x00DF, 's' }, { 0x00E6, 'a' }, { 0x00F0, 'd' },
	{ 0x00F8, 'o' }, { 0x00FE, 't' }, { 0x0110, 'D' }, { 0x0111, 'd' }, { 0x0126, 'H' }, { 0x0127, 'h' }, { 0x0131, 'i' },
	{ 0x0141, 'L' }, { 0x0142, 'l' }, { 0x0152, 'O' }, { 0x0153, 'o' }, { 0x0166, 'T' }, { 0x0167, 't' },
};

const std::uint16_t* codepageTable(int codepage)
{
	return codepage == 1250 ? kWindows1250 : kWindows1252;
}

// The closest ASCII text for a character the code page does not have.
bool appendFallback(std::string& output, utf8proc_int32_t codepoint)
{
	if (codepoint == 0x2018 || codepoint == 0x2019 || codepoint == 0x2032) output.push_back('\'');
	else if (codepoint == 0x201C || codepoint == 0x201D || codepoint == 0x2033) output.push_back('"');
	else if (codepoint == 0x2013 || codepoint == 0x2014 || codepoint == 0x2212) output.push_back('-');
	else if (codepoint == 0x2026) output += "...";
	else if (codepoint == 0x2022) output.push_back('*');
	else if (utf8proc_category(codepoint) == UTF8PROC_CATEGORY_MN) return true; // a mark that did not compose
	else
	{
		for (const auto& letter : kUndecomposable)
		{
			if (letter.first != codepoint) continue;
			output.push_back(letter.second);
			return true;
		}

		utf8proc_int32_t decomposed[4];
		int boundclass = 0;
		const utf8proc_ssize_t length = utf8proc_decompose_char(codepoint, decomposed, 4, UTF8PROC_DECOMPOSE, &boundclass);
		if (length < 1 || length > 4 || decomposed[0] >= 0x80) return false;
		output.push_back(static_cast<char>(decomposed[0]));
	}
	return true;
}
}

std::string utf8ToCodepage(const std::string& text, int codepage)
{
	// YouTube titles are often decomposed ("e" + U+0328): compose them into
	// one letter first so they can be encoded.
	utf8proc_uint8_t* composed = nullptr;
	const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.c_str()), static_cast<utf8proc_ssize_t>(text.size()), &composed,
		static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
	// Not UTF-8: the text is already in a code page.
	if (length < 0) return text;
	const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owner(composed, &std::free);

	const std::uint16_t* const table = codepageTable(codepage);
	std::string output;
	output.reserve(static_cast<std::size_t>(length));
	for (utf8proc_ssize_t index = 0; index < length;)
	{
		utf8proc_int32_t codepoint = 0;
		index += utf8proc_iterate(composed + index, length - index, &codepoint);
		if (codepoint < 0x80)
		{
			output.push_back(static_cast<char>(codepoint));
			continue;
		}
		const std::uint16_t* const found = std::find(table, table + 128, static_cast<std::uint16_t>(codepoint));
		if (codepoint <= 0xFFFF && found != table + 128) output.push_back(static_cast<char>(0x80 + (found - table)));
		else if (!appendFallback(output, codepoint)) output.push_back('?');
	}
	return output;
}

bool isSupportedCodepage(int codepage)
{
	return codepage == 1250 || codepage == 1252;
}

std::string trim(const std::string& value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

std::string randomToken(std::size_t length)
{
	static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	static std::mutex mutex;
	static std::mt19937_64 generator = []
	{
		std::random_device device;
		std::seed_seq seed { device(), device(), device(), device() };
		return std::mt19937_64(seed);
	}();

	std::lock_guard<std::mutex> lock(mutex);
	std::uniform_int_distribution<std::size_t> pick(0, sizeof(kAlphabet) - 2);
	std::string token(length, 'a');
	for (char& c : token) c = kAlphabet[pick(generator)];
	return token;
}
}
