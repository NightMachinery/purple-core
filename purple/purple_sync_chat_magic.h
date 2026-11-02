/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_peer_token.h"
#include "purple/purple_sync_chat.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Purple {

// The sync chat every device finds or creates itself; see docs/sync-chat-magic.md.
inline constexpr auto kMagicSyncChatTitle = "Magic Purple Sync";

// Telegram methods whose transient waits the binding remembers by name.
inline constexpr auto kSyncChatAllChatsMethod = "messages.getAllChats";
inline constexpr auto kSyncChatCreateMethod = "channels.createChannel";
inline constexpr auto kSyncChatFullInfoMethod = "channels.getFullChannel";

// How often a bound magic chat's full info is read again before an automatic post.
inline constexpr int64_t kSyncChatVerifySeconds = 24 * 60 * 60;
// WHY: an uncertain creation may still arrive through updates;
// creating again inside this window would make a duplicate chat.
inline constexpr int64_t kSyncChatCreateUncertainSeconds = 24 * 60 * 60;

[[nodiscard]] bool IsMagicSyncChatTitle(const QString &title);

// Real problems: each pauses automatic sync until the user acts and is shown once.
enum class SyncChatProblem {
	None,
	ChatGone,
	ChatLeft,
	NotMegagroup,
	NotCreator,
	ExtraMember,
	ExtraAdmin,
	PublicUsername,
	Restricted,
	CannotPost,
	ForeignRecord,
	ForwardedRecord,
	ViaBotRecord,
	CreateLimit,
	MagicIneligible,
	SpaceMismatch,
};

// Stable lowercase codes, used in the binding file and in diagnostics.
[[nodiscard]] QString SyncChatProblemCode(SyncChatProblem problem);
[[nodiscard]] std::optional<SyncChatProblem> ParseSyncChatProblemCode(
	const QString &code);

// What a client knows about one chat; counts stay unknown until a full-info read.
struct MagicChatCandidate {
	uint64_t channelId = 0;
	QString title;
	bool member = true;
	bool megagroup = false;
	bool gigagroup = false;
	SyncChatFacts facts;
};

enum class MagicChatVerdict {
	Eligible,
	// Everything known passes, but member or admin counts are unknown.
	Unverified,
	Ineligible,
};

struct MagicChatCheck {
	MagicChatVerdict verdict = MagicChatVerdict::Ineligible;
	SyncChatProblem problem = SyncChatProblem::None;
	SyncChatRefusal refusal = SyncChatRefusal::None;
};

// WHY: the title is not checked here, so a bound chat keeps its role after a rename.
[[nodiscard]] MagicChatCheck CheckMagicChat(const MagicChatCandidate &chat);

// The lowest (oldest) eligible or unverified channel id, the same on every device.
[[nodiscard]] std::optional<uint64_t> PickMagicChat(
	const std::vector<MagicChatCandidate> &chats);

enum class SyncChatBindingMode {
	None,
	Magic,
	Manual,
};

enum class SyncChatCreateStep {
	None,
	Requested,
	Uncertain,
	Created,
	Archived,
	Muted,
};

enum class SyncChatMigrationStep {
	None,
	Moved,
	Published,
	Joined,
	Done,
};

// sync/chat.json: this device's sync chat for one account.
struct SyncChatBinding {
	int version = 1;
	SyncChatBindingMode mode = SyncChatBindingMode::None;
	std::optional<PeerToken> chat;
	int64_t verifiedAt = 0;
	// Telegram method name -> unix time before which it must not be sent.
	std::map<QString, int64_t> waits;
	SyncChatCreateStep createStep = SyncChatCreateStep::None;
	int64_t createAt = 0;
	std::optional<PeerToken> created;
	SyncChatProblem problem = SyncChatProblem::None;
	bool problemShown = false;
	SyncChatMigrationStep migration = SyncChatMigrationStep::None;
	// Opaque per-login authority supplied by the client, never logged.
	QString login;
	QJsonObject preserved;

	[[nodiscard]] bool operator==(const SyncChatBinding &) const = default;
};

[[nodiscard]] std::optional<QByteArray> SerializeSyncChatBinding(
	const SyncChatBinding &binding);
[[nodiscard]] std::optional<SyncChatBinding> ParseSyncChatBinding(
	const QByteArray &bytes);

// A recorded deadline is only ever extended, never shortened.
void RecordSyncChatWait(
	SyncChatBinding &binding,
	const QString &method,
	int64_t retryAt);
// The time before which `method` must not be sent, or 0.
[[nodiscard]] int64_t SyncChatWaitUntil(
	const SyncChatBinding &binding,
	const QString &method,
	int64_t now);

// A different problem than the recorded one is shown again.
void RaiseSyncChatProblem(SyncChatBinding &binding, SyncChatProblem problem);
// The user acted; the next lookup decides afresh.
void ClearSyncChatProblem(SyncChatBinding &binding);

enum class SyncChatLookupAction {
	Use,
	// Read fresh full info of `chat`, then call CompleteSyncChatVerify.
	Verify,
	// Wait for a list, a load or the deadline in `retryAt`.
	Wait,
	// Read messages.getAllChats once and plan again with its result.
	ProveAbsent,
	Create,
	Problem,
};

enum class SyncChatBoundState {
	// The bound chat is not in the local cache yet; the client loads it.
	Unknown,
	Present,
	// Deleted, left, kicked or banned.
	Gone,
};

struct SyncChatLookupInput {
	SyncChatBinding binding;
	SyncChatBoundState boundState = SyncChatBoundState::Unknown;
	// The bound chat's local facts when Present.
	MagicChatCandidate bound;
	// Chats titled kMagicSyncChatTitle in the local lists.
	std::vector<MagicChatCandidate> visible;
	// Chats titled kMagicSyncChatTitle in a messages.getAllChats reply.
	std::optional<std::vector<MagicChatCandidate>> allChats;
	// The trigger needs fresh evidence (manual check or an automatic post).
	bool fresh = false;
	int64_t now = 0;
};

struct SyncChatLookupPlan {
	SyncChatLookupAction action = SyncChatLookupAction::Wait;
	std::optional<PeerToken> chat;
	SyncChatProblem problem = SyncChatProblem::None;
	// Show the problem now: it has not been shown before.
	bool show = false;
	int64_t retryAt = 0;
	// The plan's chat replaces the bound one.
	bool rebind = false;
};

[[nodiscard]] SyncChatLookupPlan PlanSyncChatLookup(
	const SyncChatLookupInput &input);

// Binds `chat` after a Verify read, or raises and returns its problem.
SyncChatProblem CompleteSyncChatVerify(
	SyncChatBinding &binding,
	const MagicChatCandidate &chat,
	int64_t now);

}
