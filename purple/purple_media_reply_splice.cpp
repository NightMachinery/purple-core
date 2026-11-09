#include "purple/purple_media_reply_splice.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <algorithm>
#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

QString Quote(const QString &text) {
	const auto bytes = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
	return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}

QJsonObject RowJson(const MediaReplyRow &row) {
	return {
		{u"voice"_q, row.voice ? QJsonValue(int(*row.voice)) : QJsonValue()},
		{u"round"_q, row.round ? QJsonValue(int(*row.round)) : QJsonValue()},
		{u"round_follow_voice"_q, row.roundFollow ? QJsonValue(*row.roundFollow) : QJsonValue()} };
}

int Offset(const QByteArray &text, int line, int column) {
	auto offset = 0;
	for (auto i = 1; i < line; ++i) {
		offset = text.indexOf('\n', offset);
		if (offset < 0) return -1;
		++offset;
	}
	return offset + column - 1;
}

} // namespace

SpliceResult SetMediaReplyValue(
		const QString &text,
		const QString &path,
		const QString &row,
		const QString &key,
		const QJsonValue &value) {
	const auto refuse = [&](const QString &why) { return SpliceResult{text, false, why}; };
	auto tableName = u"media_reply"_q;
	auto parts = QStringList{tableName};
	if (!row.isEmpty()) {
		if (row == u"private"_q || row == u"groups"_q || row == u"channels"_q) {
			tableName += '.' + row;
			parts.push_back(row);
		} else if (row.startsWith(u"sets."_q)) {
			const auto name = row.mid(5);
			if (name.trimmed().isEmpty() || name.size() > 128
				|| std::any_of(name.begin(), name.end(), [](QChar ch) { return ch.unicode() < 0x20 || ch.unicode() == 0x7f; })) {
				return refuse(u"A chat set needs a name of at most 128 characters."_q);
			}
			tableName += u".sets."_q + Quote(name);
			parts.push_back(u"sets"_q);
			parts.push_back(name);
		} else {
			return refuse(u"Invalid media auto-reply row."_q);
		}
	}
	auto encoded = QString();
	if (value.isNull() && !row.isEmpty()
		&& (key == u"voice"_q || key == u"round"_q || key == u"round_follow_voice"_q)) {
		encoded = QString();
	} else if (row.isEmpty() && key == u"text"_q && value.isString()
		&& !value.toString().trimmed().isEmpty() && value.toString().size() <= 4096) {
		encoded = Quote(value.toString());
	} else if (!row.isEmpty() && (key == u"voice"_q || key == u"round"_q)
		&& value.isString() && ParseMediaReplyMode(value.toString())) {
		encoded = Quote(value.toString());
	} else if (!row.isEmpty() && key == u"round_follow_voice"_q && value.isBool()) {
		encoded = value.toBool() ? u"true"_q : u"false"_q;
	} else if (row.startsWith(u"sets."_q) && key == u"peers"_q && value.isArray()) {
		auto peers = QStringList();
		for (const auto entry : value.toArray()) {
			const auto peer = entry.isString() ? ParsePeerToken(entry.toString()) : std::nullopt;
			if (!peer || peer->kind == PeerTokenKind::Self) return refuse(u"Invalid chat identity."_q);
			peers.push_back(Quote(PeerTokenText(*peer).value()));
		}
		encoded = '[' + peers.join(u", "_q) + ']';
	} else {
		return refuse(u"Invalid media auto-reply setting."_q);
	}
	const auto source = text.toUtf8();
	auto parsed = toml::parse(std::string_view(source.constData(), source.size()), path.toStdString());
	if (!parsed) return refuse(u"Settings could not be parsed."_q);
	const auto findTable = [&](toml::table &root) -> toml::table* {
		auto table = &root;
		for (const auto &part : parts) {
			const auto node = table->get(part.toStdString());
			table = node ? node->as_table() : nullptr;
			if (!table) return nullptr;
		}
		return table;
	};
	const auto keyName = key.toStdString();
	const auto table = findTable(parsed.table());
	if (table && table->is_inline()) return refuse(u"Expand this inline table before editing it."_q);
	const auto node = table ? table->get(keyName) : nullptr;
	auto edited = source;
	if (value.isNull()) {
		if (!node) return {text, false, {}};
		const auto from = Offset(source, node->source().begin.line, 1);
		const auto endValue = Offset(source, node->source().end.line, node->source().end.column);
		const auto nextLine = source.indexOf('\n', endValue);
		const auto till = nextLine < 0 ? source.size() : nextLine + 1;
		if (from < 0 || till < from) return refuse(u"Invalid value span."_q);
		const auto tail = source.mid(endValue, till - endValue).trimmed();
		edited.replace(from, till - from, tail.startsWith('#') ? tail + '\n' : QByteArray());
	} else if (node) {
		const auto at = node->source();
		const auto from = Offset(source, at.begin.line, at.begin.column);
		const auto till = Offset(source, at.end.line, at.end.column);
		if (from < 0 || till < from || till > source.size()) return refuse(u"Invalid value span."_q);
		edited.replace(from, till - from, encoded.toUtf8());
	} else {
		const auto assignment = (key + u" = "_q + encoded + '\n').toUtf8();
		auto inserted = false;
		if (table) {
			const auto from = Offset(source, table->source().begin.line, 1);
			const auto till = from < 0 ? -1 : source.indexOf('\n', from);
			const auto header = till < 0 ? QByteArray() : source.mid(from, till - from);
			const auto probe = header + "\n__purple_media_probe = true\n";
			auto headerParse = toml::parse(std::string_view(probe.constData(), probe.size()));
			const auto headerTable = headerParse ? findTable(headerParse.table()) : nullptr;
			if (headerTable && (*headerTable)["__purple_media_probe"].value<bool>().value_or(false)) {
				edited.insert(till + 1, assignment);
				inserted = true;
			}
		}
		if (!inserted) {
			if (!edited.isEmpty() && !edited.endsWith('\n')) edited += '\n';
			edited += '\n' + ('[' + tableName + u"]\n"_q).toUtf8() + assignment;
		}
	}
	auto verify = toml::parse(std::string_view(edited.constData(), edited.size()));
	if (!verify) return refuse(u"The edit would not parse back."_q);
	const auto backTable = findTable(verify.table());
	const auto back = backTable ? backTable->get(keyName) : nullptr;
	if (value.isNull() ? bool(back) : !back) return refuse(u"The edited setting did not match the requested change."_q);
	return {QString::fromUtf8(edited), edited != source, {}};
}

QJsonObject MediaReplySettingsJson(const Settings &settings) {
	auto defaults = QJsonArray();
	for (const auto &row : settings.mediaReply.defaults) defaults.push_back(RowJson(row));
	auto sets = QJsonArray();
	for (const auto &set : settings.mediaReply.sets) {
		auto object = RowJson(set.row);
		object.insert(u"name"_q, set.name);
		object.insert(u"list"_q, set.list);
		auto peers = QJsonArray();
		for (const auto &peer : set.peers) peers.push_back(PeerTokenText(peer).value_or(QString()));
		object.insert(u"peers"_q, peers);
		sets.push_back(object);
	}
	return {{u"text"_q, settings.mediaReply.text}, {u"defaults"_q, defaults}, {u"sets"_q, sets}};
}

} // namespace Purple
