/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_json.h"

#include <QtCore/QSet>
#include <QtCore/QString>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Purple {
namespace {

constexpr auto kMaximumDepth = 128;
constexpr auto kMaximumBytes = 4 * 1024 * 1024;
constexpr auto kMaximumSafeInteger = "9007199254740991";

class Parser {
public:
	explicit Parser(const QByteArray &input) : _input(input) {
	}

	[[nodiscard]] SyncJsonResult Parse() {
		if (_input.size() > kMaximumBytes) {
			return { {}, SyncJsonErrorKind::SizeLimit, kMaximumBytes };
		}
		SkipSpace();
		auto canonical = QByteArray();
		if (!Value(canonical, 0)) {
			return { {}, _error, _errorOffset };
		}
		SkipSpace();
		if (_position != _input.size()) {
			return { {}, SyncJsonErrorKind::Syntax, _position };
		}
		return { std::move(canonical), SyncJsonErrorKind::None, 0 };
	}

private:
	[[nodiscard]] bool Fail(SyncJsonErrorKind error, qsizetype offset) {
		_error = error;
		_errorOffset = offset;
		return false;
	}

	[[nodiscard]] bool Take(char character) {
		if (_position < _input.size() && _input[_position] == character) {
			++_position;
			return true;
		}
		return false;
	}

	void SkipSpace() {
		while (_position < _input.size()) {
			const auto character = _input[_position];
			if (character != ' ' && character != '\t'
				&& character != '\n' && character != '\r') {
				break;
			}
			++_position;
		}
	}

	[[nodiscard]] bool Hex(unsigned &value) {
		if (_input.size() - _position < 4) {
			return Fail(SyncJsonErrorKind::Syntax, _position);
		}
		value = 0;
		for (auto index = 0; index != 4; ++index) {
			const auto character = _input[_position++];
			const auto digit = (character >= '0' && character <= '9')
				? (character - '0')
				: (character >= 'a' && character <= 'f')
				? (character - 'a' + 10)
				: (character >= 'A' && character <= 'F')
				? (character - 'A' + 10)
				: -1;
			if (digit < 0) {
				return Fail(SyncJsonErrorKind::Syntax, _position - 1);
			}
			value = (value << 4) | digit;
		}
		return true;
	}

	[[nodiscard]] bool Utf8(QString &value) {
		const auto start = _position;
		const auto first = static_cast<unsigned char>(_input[_position++]);
		if (first < 0x80) {
			value.append(QChar(first));
			return true;
		}
		const auto length = (first >= 0xC2 && first <= 0xDF)
			? 2 : (first >= 0xE0 && first <= 0xEF)
			? 3 : (first >= 0xF0 && first <= 0xF4)
			? 4 : 0;
		if (!length || _input.size() - start < length) {
			return Fail(SyncJsonErrorKind::InvalidUtf8, start);
		}
		auto scalar = unsigned(first & ((1u << (7 - length)) - 1));
		for (auto index = 1; index != length; ++index) {
			const auto next = static_cast<unsigned char>(_input[_position++]);
			if ((next & 0xC0) != 0x80) {
				return Fail(SyncJsonErrorKind::InvalidUtf8, _position - 1);
			}
			scalar = (scalar << 6) | (next & 0x3F);
		}
		if ((length == 3 && scalar < 0x800)
			|| (length == 4 && scalar < 0x10000)
			|| (scalar >= 0xD800 && scalar <= 0xDFFF)
			|| scalar > 0x10FFFF) {
			return Fail(SyncJsonErrorKind::InvalidUtf8, start);
		}
		if (scalar <= 0xFFFF) {
			value.append(QChar(scalar));
		} else {
			scalar -= 0x10000;
			value.append(QChar(0xD800 + (scalar >> 10)));
			value.append(QChar(0xDC00 + (scalar & 0x3FF)));
		}
		return true;
	}

	[[nodiscard]] bool String(QString &value) {
		if (!Take('"')) {
			return Fail(SyncJsonErrorKind::Syntax, _position);
		}
		while (_position < _input.size()) {
			const auto start = _position;
			const auto character = static_cast<unsigned char>(_input[_position]);
			if (character == '"') {
				++_position;
				return true;
			}
			if (character == '\\') {
				++_position;
				if (_position == _input.size()) {
					return Fail(SyncJsonErrorKind::Syntax, _position);
				}
				const auto escape = _input[_position++];
				switch (escape) {
				case '"': value.append(u'"'); break;
				case '\\': value.append(u'\\'); break;
				case '/': value.append(u'/'); break;
				case 'b': value.append(u'\b'); break;
				case 'f': value.append(u'\f'); break;
				case 'n': value.append(u'\n'); break;
				case 'r': value.append(u'\r'); break;
				case 't': value.append(u'\t'); break;
				case 'u': {
					auto first = 0u;
					if (!Hex(first)) {
						return false;
					}
					if (first >= 0xDC00 && first <= 0xDFFF) {
						return Fail(SyncJsonErrorKind::InvalidUnicode, start);
					}
					if (first >= 0xD800 && first <= 0xDBFF) {
						if (!Take('\\') || !Take('u')) {
							return Fail(SyncJsonErrorKind::InvalidUnicode, start);
						}
						auto second = 0u;
						if (!Hex(second)) {
							return false;
						}
						if (second < 0xDC00 || second > 0xDFFF) {
							return Fail(SyncJsonErrorKind::InvalidUnicode, start);
						}
						value.append(QChar(first));
						value.append(QChar(second));
					} else {
						value.append(QChar(first));
					}
				} break;
				default: return Fail(SyncJsonErrorKind::Syntax, _position - 1);
				}
			} else if (character < 0x20) {
				return Fail(SyncJsonErrorKind::Syntax, start);
			} else if (!Utf8(value)) {
				return false;
			}
		}
		return Fail(SyncJsonErrorKind::Syntax, _position);
	}

	[[nodiscard]] static QByteArray Quote(const QString &value) {
		auto result = QByteArray("\"");
		constexpr auto hex = "0123456789abcdef";
		for (auto index = qsizetype(0); index != value.size(); ++index) {
			const auto code = value[index].unicode();
			switch (code) {
			case '"': result.append("\\\""); break;
			case '\\': result.append("\\\\"); break;
			case '\b': result.append("\\b"); break;
			case '\f': result.append("\\f"); break;
			case '\n': result.append("\\n"); break;
			case '\r': result.append("\\r"); break;
			case '\t': result.append("\\t"); break;
			default:
				if (code < 0x20) {
					result.append("\\u00");
					result.append(hex[code >> 4]);
					result.append(hex[code & 15]);
				} else {
					const auto count = (code >= 0xD800 && code <= 0xDBFF)
						? 2 : 1;
					result.append(value.mid(index, count).toUtf8());
					index += count - 1;
				}
			}
		}
		result.append('"');
		return result;
	}

	[[nodiscard]] bool Number(QByteArray &result) {
		const auto start = _position;
		const auto negative = Take('-');
		if (_position == _input.size()) {
			return Fail(SyncJsonErrorKind::Syntax, _position);
		}
		auto digits = QByteArray();
		if (Take('0')) {
			digits.append('0');
			if (_position < _input.size()
				&& _input[_position] >= '0' && _input[_position] <= '9') {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
		} else {
			if (_input[_position] < '1' || _input[_position] > '9') {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
			while (_position < _input.size()
				&& _input[_position] >= '0' && _input[_position] <= '9') {
				digits.append(_input[_position++]);
			}
		}
		auto fractionLength = qsizetype(0);
		if (Take('.')) {
			if (_position == _input.size()
				|| _input[_position] < '0' || _input[_position] > '9') {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
			while (_position < _input.size()
				&& _input[_position] >= '0' && _input[_position] <= '9') {
				digits.append(_input[_position++]);
				++fractionLength;
			}
		}
		auto exponent = int64_t(0);
		if (Take('e') || Take('E')) {
			const auto exponentNegative = Take('-');
			if (!exponentNegative) {
				(void)Take('+');
			}
			if (_position == _input.size()
				|| _input[_position] < '0' || _input[_position] > '9') {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
			while (_position < _input.size()
				&& _input[_position] >= '0' && _input[_position] <= '9') {
				exponent = std::min<int64_t>(
					1000000000, exponent * 10 + (_input[_position++] - '0'));
			}
			if (exponentNegative) {
				exponent = -exponent;
			}
		}
		auto significant = qsizetype(0);
		while (significant < digits.size() && digits[significant] == '0') {
			++significant;
		}
		if (significant == digits.size()) {
			result = "0";
			return true;
		}
		digits.remove(0, significant);
		const auto scale = exponent - fractionLength;
		if (scale < 0) {
			const auto needed = -scale;
			if (needed > digits.size()) {
				return Fail(SyncJsonErrorKind::NonInteger, start);
			}
			for (auto index = qsizetype(0); index != needed; ++index) {
				if (digits[digits.size() - 1 - index] != '0') {
					return Fail(SyncJsonErrorKind::NonInteger, start);
				}
			}
			digits.chop(needed);
		} else if (scale > 16 || digits.size() + scale > 16) {
			return Fail(SyncJsonErrorKind::NumberRange, start);
		} else {
			digits.append(QByteArray(scale, '0'));
		}
		if (digits.size() > 16
			|| (digits.size() == 16 && digits > kMaximumSafeInteger)) {
			return Fail(SyncJsonErrorKind::NumberRange, start);
		}
		result = negative ? QByteArray("-") + digits : digits;
		return true;
	}

	[[nodiscard]] bool Array(QByteArray &result, int depth) {
		++_position;
		result.append('[');
		SkipSpace();
		if (Take(']')) {
			result.append(']');
			return true;
		}
		while (true) {
			auto item = QByteArray();
			if (!Value(item, depth + 1)) {
				return false;
			}
			result.append(item);
			SkipSpace();
			if (Take(']')) {
				result.append(']');
				return true;
			}
			if (!Take(',')) {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
			result.append(',');
			SkipSpace();
		}
	}

	[[nodiscard]] bool Object(QByteArray &result, int depth) {
		++_position;
		SkipSpace();
		if (Take('}')) {
			result = "{}";
			return true;
		}
		auto members = std::vector<std::pair<QString, QByteArray>>();
		auto names = QSet<QString>();
		while (true) {
			const auto keyOffset = _position;
			auto name = QString();
			if (!String(name)) {
				return false;
			}
			if (names.contains(name)) {
				return Fail(SyncJsonErrorKind::DuplicateKey, keyOffset);
			}
			names.insert(name);
			SkipSpace();
			if (!Take(':')) {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
			SkipSpace();
			auto value = QByteArray();
			if (!Value(value, depth + 1)) {
				return false;
			}
			members.emplace_back(std::move(name), std::move(value));
			SkipSpace();
			if (Take('}')) {
				break;
			}
			if (!Take(',')) {
				return Fail(SyncJsonErrorKind::Syntax, _position);
			}
			SkipSpace();
		}
		std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) {
			const auto left = a.first.utf16();
			const auto right = b.first.utf16();
			return std::lexicographical_compare(
				left, left + a.first.size(), right, right + b.first.size());
		});
		result.append('{');
		for (const auto &[name, value] : members) {
			if (result.size() > 1) {
				result.append(',');
			}
			result.append(Quote(name));
			result.append(':');
			result.append(value);
		}
		result.append('}');
		return true;
	}

	[[nodiscard]] bool Value(QByteArray &result, int depth) {
		if (depth > kMaximumDepth) {
			return Fail(SyncJsonErrorKind::DepthLimit, _position);
		}
		if (_position == _input.size()) {
			return Fail(SyncJsonErrorKind::Syntax, _position);
		}
		const auto character = _input[_position];
		if (character == '{') {
			return Object(result, depth);
		} else if (character == '[') {
			return Array(result, depth);
		} else if (character == '"') {
			auto value = QString();
			if (!String(value)) {
				return false;
			}
			result = Quote(value);
			return true;
		} else if (character == '-' || (character >= '0' && character <= '9')) {
			return Number(result);
		}
		for (const auto literal : { "true", "false", "null" }) {
			const auto length = qsizetype(std::char_traits<char>::length(literal));
			if (_input.mid(_position, length) == literal) {
				_position += length;
				result = literal;
				return true;
			}
		}
		return Fail(SyncJsonErrorKind::Syntax, _position);
	}

	const QByteArray &_input;
	qsizetype _position = 0;
	SyncJsonErrorKind _error = SyncJsonErrorKind::None;
	qsizetype _errorOffset = 0;

};

} // namespace

SyncJsonResult CanonicalizeSyncJson(const QByteArray &json) {
	return Parser(json).Parse();
}

} // namespace Purple
