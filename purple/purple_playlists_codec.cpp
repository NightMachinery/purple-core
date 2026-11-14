/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_codec.h"

#include "purple/purple_playlists_codec_detail.h"

namespace Purple {

PlaylistCodecResult EncodePlaylistBytes(
		const QByteArray &plainBytes,
		const PlaylistCodecCaps &caps) {
	return PlaylistsCodecDetail::EncodePlaylistBytesWithFactory(
		plainBytes,
		caps,
		PlaylistsCodecDetail::QByteArrayOutputFactory{});
}

PlaylistCodecResult DecodePlaylistBytes(
		const QByteArray &gzipBytes,
		const PlaylistCodecCaps &caps) {
	return PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
		gzipBytes,
		caps,
		PlaylistsCodecDetail::QByteArrayOutputFactory{});
}

} // namespace Purple
