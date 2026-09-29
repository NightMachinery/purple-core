/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_config_sync.h"

#include "purple/purple_state.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>

namespace Purple {
namespace {

constexpr auto kLineageLimit = 64;

[[nodiscard]] bool Decimal(const QString &value) {
	if (value.isEmpty() || (value.size() > 1 && value.front() == u'0')) {
		return false;
	}
	for (const auto character : value) {
		if (character < u'0' || character > u'9') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool FingerprintValid(const QString &value) {
	const auto colon = value.indexOf(u':');
	if (colon < 0 || !Decimal(value.left(colon))
		|| value.size() != colon + 65) {
		return false;
	}
	auto ok = false;
	value.left(colon).toULongLong(&ok);
	if (!ok) {
		return false;
	}
	for (auto index = colon + 1; index != value.size(); ++index) {
		const auto character = value[index];
		if (!((character >= u'0' && character <= u'9')
			|| (character >= u'a' && character <= u'f'))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::optional<ConfigVersionKey> ParseKey(const QString &key) {
	const auto dot = key.indexOf(u'.');
	if (dot < 0 || !Decimal(key.left(dot))) {
		return std::nullopt;
	}
	const auto fingerprint = key.mid(dot + 1);
	if (!FingerprintValid(fingerprint)) {
		return std::nullopt;
	}
	auto ok = false;
	const auto generation = key.left(dot).toULongLong(&ok);
	if (!ok || !generation) {
		return std::nullopt;
	}
	return ConfigVersionKey{ generation, fingerprint };
}

[[nodiscard]] bool ValidLineage(
		const QString &key,
		const std::vector<QString> &lineage) {
	const auto parts = ParseKey(key);
	if (!parts || lineage.size() > kLineageLimit) {
		return false;
	}
	for (auto index = size_t(0); index != lineage.size(); ++index) {
		const auto ancestor = ParseKey(lineage[index]);
		if (!ancestor || ancestor->generation >= parts->generation
			|| std::find(lineage.begin(), lineage.begin() + index,
				lineage[index]) != lineage.begin() + index) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool Has(
		const std::vector<QString> &keys,
		const QString &key) {
	return std::find(keys.begin(), keys.end(), key) != keys.end();
}

[[nodiscard]] bool Meets(
		const std::vector<QString> &lineage,
		const std::vector<QString> &keys) {
	return std::any_of(keys.begin(), keys.end(), [&](const auto &key) {
		return Has(lineage, key);
	});
}

[[nodiscard]] bool StateValid(
		const QString &localFp,
		const ConfigSyncState &state) {
	return FingerprintValid(localFp)
		&& !state.space.isEmpty()
		&& (state.base.isEmpty()
			|| ValidLineage(state.base, state.baseLineage))
		&& (!state.base.isEmpty() || state.baseLineage.empty())
		&& state.equiv.size() <= 16
		&& (state.pending.isEmpty() || IsConfigVersionKey(state.pending))
		&& std::all_of(state.equiv.begin(), state.equiv.end(),
			[](const auto &key) { return IsConfigVersionKey(key); });
}

[[nodiscard]] bool Unjoined(const ConfigSyncState &state) {
	return state.install.isEmpty()
		&& !state.space.isEmpty()
		&& state.base.isEmpty()
		&& state.baseLineage.empty()
		&& state.equiv.empty()
		&& state.pending.isEmpty()
		&& state.seenSeq.empty();
}

[[nodiscard]] QString Fingerprint(const QString &key) {
	const auto parts = ParseKey(key);
	return parts ? parts->fingerprint : QString();
}

[[nodiscard]] uint64_t Generation(const QString &key) {
	const auto parts = ParseKey(key);
	return parts ? parts->generation : 0;
}

[[nodiscard]] bool Precedes(const ConfigHead &a, const ConfigHead &b) {
	const auto first = Generation(a.key);
	const auto second = Generation(b.key);
	if (first != second) {
		return first > second;
	} else if (a.seq != b.seq) {
		return a.seq > b.seq;
	} else if (a.install != b.install) {
		return a.install < b.install;
	}
	return a.key < b.key;
}

[[nodiscard]] std::vector<ConfigHead> HeadsOf(
		const ConfigClassification &classification,
		std::initializer_list<ConfigHeadKind> kinds) {
	auto result = std::vector<ConfigHead>();
	for (const auto &outcome : classification.heads) {
		if (std::find(kinds.begin(), kinds.end(), outcome.kind)
			!= kinds.end()) {
			result.push_back(outcome.head);
		}
	}
	std::sort(result.begin(), result.end(), Precedes);
	return result;
}

[[nodiscard]] std::vector<ConfigHead> Representatives(
		const std::vector<ConfigHead> &sorted) {
	auto result = std::vector<ConfigHead>();
	auto fingerprints = std::vector<QString>();
	for (const auto &head : sorted) {
		const auto fp = Fingerprint(head.key);
		if (!Has(fingerprints, fp)) {
			fingerprints.push_back(fp);
			result.push_back(head);
		}
	}
	return result;
}

[[nodiscard]] ConfigClassification Classify(
		const QString &localFp,
		const ConfigSyncState &state,
		const std::vector<ConfigHead> &remoteHeads,
		bool unjoined) {
	auto result = ConfigClassification();
	if (!StateValid(localFp, state)
		|| (state.install.isEmpty() && !unjoined)) {
		result.inputValid = false;
		return result;
	}
	auto selected = std::map<QString, ConfigHead>();
	auto ambiguous = std::set<QString>();
	for (const auto &head : remoteHeads) {
		if (head.space != state.space
			|| (!unjoined && head.install == state.install)) {
			continue;
		}
		const auto seen = state.seenSeq.find(head.install);
		if (seen != state.seenSeq.end() && head.seq <= seen->second) {
			continue;
		}
		auto existing = selected.find(head.install);
		if (existing == selected.end() || head.seq > existing->second.seq) {
			selected[head.install] = head;
			ambiguous.erase(head.install);
		} else if (head.seq == existing->second.seq
			&& (head.key != existing->second.key
				|| head.lineage != existing->second.lineage)) {
			ambiguous.insert(head.install);
		}
	}
	for (auto it = selected.begin(); it != selected.end();) {
		if (ambiguous.contains(it->first)
			|| !it->second.seq
			|| !ValidLineage(it->second.key, it->second.lineage)) {
			result.heads.push_back({ it->second, ConfigHeadKind::Invalid });
			it = selected.erase(it);
		} else {
			++it;
		}
	}
	auto maximal = std::vector<ConfigHead>();
	for (const auto &entry : selected) {
		const auto &install = entry.first;
		const auto &head = entry.second;
		const auto behind = std::any_of(selected.begin(), selected.end(),
			[&](const auto &other) {
				return other.first != install
					&& Has(other.second.lineage, head.key);
			});
		if (behind) {
			result.heads.push_back({ head, ConfigHeadKind::Stale });
		} else {
			maximal.push_back(head);
		}
	}
	auto fingerprints = std::vector<QString>();
	auto matchesLocal = false;
	for (const auto &head : maximal) {
		const auto fp = ParseKey(head.key)->fingerprint;
		if (!Has(fingerprints, fp)) {
			fingerprints.push_back(fp);
		}
		matchesLocal |= (fp == localFp);
	}
	result.split = fingerprints.size() > 1 && !matchesLocal;

	auto known = state.equiv;
	if (!state.base.isEmpty()) {
		known.push_back(state.base);
	}
	const auto base = ParseKey(state.base);
	const auto clean = base && base->fingerprint == localFp;
	for (const auto &head : maximal) {
		const auto fp = ParseKey(head.key)->fingerprint;
		const auto meetsKnown = Meets(head.lineage, known);
		const auto holdsPending = !state.pending.isEmpty()
			&& Has(head.lineage, state.pending);
		auto kind = ConfigHeadKind::Unrelated;
		if (Has(state.baseLineage, head.key) || Has(known, head.key)) {
			kind = ConfigHeadKind::Stale;
		} else if (fp == localFp) {
			kind = ConfigHeadKind::Same;
		} else if (holdsPending || (clean && meetsKnown)) {
			kind = ConfigHeadKind::Ahead;
		} else if (!clean && meetsKnown) {
			kind = ConfigHeadKind::Concurrent;
		}
		result.heads.push_back({ head, kind });
	}
	return result;
}

} // namespace

std::optional<ConfigVersionKey> ParseConfigVersionKey(
		const QString &key) {
	return ParseKey(key);
}

bool IsConfigVersionKey(const QString &key) {
	return ParseConfigVersionKey(key).has_value();
}

std::optional<ConfigVersion> MakeConfigVersion(
		const QByteArray &text,
		const std::vector<ConfigVersion> &parents) {
	auto result = ConfigVersion();
	auto generation = uint64_t(0);
	auto ancestors = std::vector<QString>();
	for (const auto &parent : parents) {
		const auto key = ParseKey(parent.key);
		if (!key || !ValidLineage(parent.key, parent.lineage)
			|| key->generation == std::numeric_limits<uint64_t>::max()) {
			return std::nullopt;
		}
		generation = std::max(generation, key->generation);
		if (!Has(result.parents, parent.key)) {
			result.parents.push_back(parent.key);
			if (result.parents.size() > 2) {
				return std::nullopt;
			}
			ancestors.push_back(parent.key);
			for (const auto &ancestor : parent.lineage) {
				if (!Has(ancestors, ancestor)) {
					ancestors.push_back(ancestor);
				}
			}
		}
	}
	std::stable_sort(ancestors.begin(), ancestors.end(), [](const auto &a,
			const auto &b) {
		return ParseKey(a)->generation > ParseKey(b)->generation;
	});
	if (ancestors.size() > kLineageLimit) {
		auto retained = result.parents;
		std::stable_sort(retained.begin(), retained.end(), [](const auto &a,
				const auto &b) {
			return ParseKey(a)->generation > ParseKey(b)->generation;
		});
		for (const auto &ancestor : ancestors) {
			if (retained.size() == kLineageLimit) {
				break;
			}
			if (!Has(retained, ancestor)) {
				retained.push_back(ancestor);
			}
		}
		ancestors = std::move(retained);
	}
	result.lineage = std::move(ancestors);
	result.key = QString::number(generation + 1)
		+ u"."_q + SettingsFingerprint(text);
	return result;
}

ConfigClassification ClassifyConfig(
		const QString &localFp,
		const ConfigSyncState &state,
		const std::vector<ConfigHead> &remoteHeads) {
	return Classify(localFp, state, remoteHeads, false);
}

ConfigSyncPlan PlanConfigSync(
		const QString &localFp,
		const ConfigSyncState &state,
		const std::vector<ConfigHead> &remoteHeads) {
	auto result = ConfigSyncPlan();
	const auto unjoined = Unjoined(state);
	result.classification = Classify(localFp, state, remoteHeads, unjoined);
	const auto &classification = result.classification;
	const auto has = [&](ConfigHeadKind kind) {
		return std::any_of(
			classification.heads.begin(),
			classification.heads.end(),
			[&](const auto &outcome) { return outcome.kind == kind; });
	};
	if (!classification.inputValid || has(ConfigHeadKind::Invalid)) {
		return result;
	}
	result.same = HeadsOf(classification, { ConfigHeadKind::Same });
	const auto candidates = Representatives(HeadsOf(classification, {
		ConfigHeadKind::Ahead,
		ConfigHeadKind::Concurrent,
		ConfigHeadKind::Unrelated,
	}));
	const auto split = classification.split && candidates.size() > 1;
	const auto others = std::any_of(
		remoteHeads.begin(),
		remoteHeads.end(),
		[&](const ConfigHead &head) {
			return head.space == state.space
				&& (unjoined || head.install != state.install);
		});
	if (!state.pending.isEmpty()) {
		result.verdict = ConfigSyncVerdict::Pending;
	} else if (has(ConfigHeadKind::Concurrent) || split) {
		result.verdict = ConfigSyncVerdict::Conflict;
		result.offered = candidates;
	} else if (has(ConfigHeadKind::Unrelated)) {
		result.verdict = ConfigSyncVerdict::Choose;
		result.offered = candidates;
	} else if (has(ConfigHeadKind::Ahead)) {
		result.verdict = ConfigSyncVerdict::UpdateReady;
		result.offered = {
			HeadsOf(classification, { ConfigHeadKind::Ahead }).front(),
		};
	} else if (!result.same.empty()) {
		result.verdict = ConfigSyncVerdict::Adopt;
	} else if (state.base.isEmpty() && !others) {
		result.verdict = ConfigSyncVerdict::Empty;
	} else if (!state.base.isEmpty() && Fingerprint(state.base) != localFp) {
		result.verdict = ConfigSyncVerdict::LocalChanges;
	} else {
		result.verdict = ConfigSyncVerdict::UpToDate;
	}
	return result;
}

std::optional<ConfigSyncState> AdoptConfigHeads(
		const ConfigSyncState &state,
		const QString &localFp,
		const std::vector<ConfigHead> &heads) {
	if (heads.empty()
		|| !state.pending.isEmpty()
		|| state.install.isEmpty()
		|| !StateValid(localFp, state)) {
		return std::nullopt;
	}
	for (const auto &head : heads) {
		if (head.space != state.space
			|| head.install == state.install
			|| !head.seq
			|| !ValidLineage(head.key, head.lineage)
			|| Fingerprint(head.key) != localFp) {
			return std::nullopt;
		}
	}
	auto sorted = heads;
	std::sort(sorted.begin(), sorted.end(), Precedes);
	auto result = state;
	auto equiv = std::vector<QString>();
	if (Fingerprint(state.base) != localFp) {
		result.base = sorted.front().key;
		result.baseLineage = sorted.front().lineage;
	} else {
		equiv = state.equiv;
	}
	for (const auto &head : sorted) {
		equiv.push_back(head.key);
	}
	std::sort(equiv.begin(), equiv.end(), [](const auto &a, const auto &b) {
		const auto first = Generation(a);
		const auto second = Generation(b);
		return (first != second) ? (first > second) : (a < b);
	});
	equiv.erase(std::unique(equiv.begin(), equiv.end()), equiv.end());
	equiv.erase(
		std::remove(equiv.begin(), equiv.end(), result.base),
		equiv.end());
	if (equiv.size() > 16) {
		equiv.resize(16);
	}
	result.equiv = std::move(equiv);
	for (const auto &head : sorted) {
		auto &seen = result.seenSeq[head.install];
		seen = std::max(seen, head.seq);
	}
	return result;
}

} // namespace Purple
