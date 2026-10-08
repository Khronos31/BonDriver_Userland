#include "bondriver/Utf16.h"

namespace bondriver {

std::u16string utf8ToUtf16(const std::string &text)
{
	std::u16string out;
	out.reserve(text.size());
	size_t i = 0;
	const size_t n = text.size();
	while (i < n) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		uint32_t cp = 0;
		size_t len = 1;
		if (c < 0x80) {
			cp = c;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			len = 2;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			len = 3;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			len = 4;
		} else {
			out.push_back(0xFFFD);
			++i;
			continue;
		}
		if (i + len > n) {
			out.push_back(0xFFFD);
			break;
		}
		bool ok = true;
		for (size_t k = 1; k < len; ++k) {
			const unsigned char cc = static_cast<unsigned char>(text[i + k]);
			if ((cc & 0xC0) != 0x80) {
				ok = false;
				break;
			}
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (!ok) {
			out.push_back(0xFFFD);
			++i;
			continue;
		}
		i += len;
		if (cp <= 0xFFFF) {
			out.push_back(static_cast<char16_t>(cp));
		} else if (cp <= 0x10FFFF) {
			cp -= 0x10000;
			out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
			out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
		} else {
			out.push_back(0xFFFD);
		}
	}
	return out;
}

} // namespace bondriver
