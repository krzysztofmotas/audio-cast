/*
 *  AudioCast - synchronized YouTube radio for open.mp and SA-MP
 *  Copyright (c) 2026 devbluen
 *
 *  Server-side only: the plugin streams MP3 over HTTP and the game client
 *  plays it with PlayAudioStreamForPlayer.  Every listener hears the same
 *  moment of the song, however late they join.
 */

#if defined _audio_cast_included
	#endinput
#endif
#define _audio_cast_included

#define INVALID_RADIO_ID            (0)
// Large enough for any stream URL produced by the plugin.
#define RADIO_URL_SIZE              (160)

// Reasons passed to OnRadioTrackEnd.
#define RADIO_END_FINISHED          (0) // the song played to the end
#define RADIO_END_SKIPPED           (1) // Radio_Skip
#define RADIO_END_STOPPED           (2) // Radio_Stop
#define RADIO_END_REPLACED          (3) // a Radio_Play request finished loading

/*
 * Radios
 */

// Creates a radio and returns its ID (INVALID_RADIO_ID on failure).  The
// radio belongs to the calling script and is destroyed when it unloads.
native Radio_Create(const name[] = "");
native Radio_Destroy(radioid);
native Radio_IsValid(radioid);
native Radio_GetName(radioid, name[], size = sizeof name);

/*
 * Playback.  `query` is a YouTube link (watch, youtu.be, shorts, music...),
 * a video ID, or search text ("artist - song").  Loading happens in the
 * background; both natives return a request ID (0 on failure) that is passed
 * to the OnRadioTrack* callbacks.  A song starts playing after its first
 * seconds are ready; the rest loads while it plays.
 *
 * Timing: players hear the stream a few seconds (audiocast_burst_ms) after it
 * is produced - that delay is what keeps everyone in sync.  The getters below
 * and OnRadioTrackStart/End follow what players hear, not what the server is
 * producing, so chat messages match the audio.  Pause, skip and seek take
 * effect for the ears after the same delay.
 */

// Plays now: the current song is replaced as soon as the new one is loaded.
native Radio_Play(radioid, const query[]);
// Adds a song to the end of the queue.
native Radio_Queue(radioid, const query[]);
native Radio_Skip(radioid);
// Stops the current song and clears the queue.  Listeners stay connected.
native Radio_Stop(radioid);
native Radio_ClearQueue(radioid);
native Radio_Pause(radioid);
native Radio_Resume(radioid);
// Jumps to a position (milliseconds) - for every listener at once.
native Radio_Seek(radioid, position_ms);

native Radio_IsPlaying(radioid);
native Radio_IsPaused(radioid);
native Radio_IsLoading(radioid);
native Radio_GetPosition(radioid);   // milliseconds
native Radio_GetDuration(radioid);   // milliseconds
native Radio_GetRequestId(radioid);  // request of the current song
native Radio_GetTitle(radioid, title[], size = sizeof title);
// YouTube Music artist and song name; return 0 when the video has none.
native Radio_GetArtist(radioid, artist[], size = sizeof artist);
native Radio_GetTrack(radioid, track[], size = sizeof track);
native Radio_GetVideoId(radioid, videoid[], size = sizeof videoid);
native Radio_GetQueueSize(radioid);
// Players (and other clients) currently connected to the stream.
native Radio_GetListenerCount(radioid);
// Volume of everyone listening to the radio: 0 (mute) to 100 (default).
// Players hear the change after the burst they already hold (about
// audiocast_burst_ms).  Volume can only go down from the song's own level.
native Radio_SetVolume(radioid, volume);
native Radio_GetVolume(radioid);
// Stream URL without a player token, for VLC/browsers.  Needs
// audiocast_allow_public 1.
native Radio_GetPublicUrl(radioid, url[], size = sizeof url);

/*
 * Search.  Lists YouTube results for a text (to show in a dialog, for
 * example) without playing anything.  The results arrive in
 * OnRadioSearchResults and stay available until Radio_FreeSearch.  Play one
 * with Radio_Play/Radio_Queue and its video ID.
 */

// Returns a search ID (0 on failure).  max_results: 1 to 20.
native Radio_Search(const query[], max_results = 5);
// Number of results, or -1 while the search is still running / unknown ID.
native Radio_GetSearchCount(searchid);
native Radio_GetSearchResult(searchid, index, videoid[], title[], &duration_ms, videoid_size = sizeof videoid, title_size = sizeof title);
native Radio_GetSearchChannel(searchid, index, channel[], size = sizeof channel);
// View count as decimal digits (it can exceed a cell); returns 0 when YouTube does not report it.
native Radio_GetSearchViews(searchid, index, views[], size = sizeof views);
native Radio_FreeSearch(searchid);

/*
 * Listeners.  Use Radio_AddPlayer / Radio_RemovePlayer below; the
 * Attach/Detach natives only manage the player's private stream URL.
 */

native Radio_AttachPlayer(playerid, radioid, url[], size = sizeof url);
native Radio_DetachPlayer(playerid);
native Radio_GetPlayerRadio(playerid);
// True while the player's game client is connected to the stream.
native Radio_IsPlayerStreaming(playerid);
// One player's volume, 0 (mute) to 100 (default), on top of the radio's:
// 50 on a radio at 50 is 25.  It stays when the player changes radio and
// resets when they leave the server.
native Radio_SetPlayerVolume(playerid, volume);
native Radio_GetPlayerVolume(playerid);

/*
 * Callbacks
 */

forward OnRadioTrackLoaded(radioid, requestid, const title[], duration_ms);
forward OnRadioTrackStart(radioid, requestid, const title[], duration_ms);
forward OnRadioTrackEnd(radioid, requestid, reason);
forward OnRadioTrackError(radioid, requestid, const query[], const error[]);
forward OnRadioListenerConnect(radioid, playerid);
forward OnRadioListenerDisconnect(radioid, playerid);
// count: number of results (0 with error[] set when the search failed).
forward OnRadioSearchResults(searchid, count, const error[]);

/*
 * Cinemas: a YouTube video on a screen in the game, at the same moment for
 * every player.  The video is downloaded as YouTube sends it (VP9 picture,
 * Opus sound, kept in <data_dir>/cinema up to audiocast_cinema_cache_mb) and
 * shown by a page the plugin serves: open Cinema_GetPageUrl in a samp-cef
 * browser on an object (cef_create_ext_browser + cef_append_to_object).  The
 * page follows the cinema's clock by itself; nothing is sent to the players.
 */

#define INVALID_CINEMA_ID           (0)
#define CINEMA_URL_SIZE             (128)

// Creates a cinema and returns its ID.  It belongs to the calling script and
// is destroyed when it unloads.
native Cinema_Create(const name[] = "");
native Cinema_Destroy(cinemaid);
native Cinema_IsValid(cinemaid);
// A YouTube link, video ID or search.  Returns a request ID (0 on failure);
// the film replaces what is on the screen once it is downloaded
// (OnCinemaFilmStart or OnCinemaFilmError).  Live streams are not supported.
native Cinema_Play(cinemaid, const query[]);
// Takes the film off the screen (and forgets a Cinema_Play still loading).
native Cinema_Stop(cinemaid);
native Cinema_Pause(cinemaid);
native Cinema_Resume(cinemaid);
native Cinema_Seek(cinemaid, position_ms);
native Cinema_IsPlaying(cinemaid);
native Cinema_IsPaused(cinemaid);
native Cinema_IsLoading(cinemaid);
native Cinema_GetPosition(cinemaid); // milliseconds
native Cinema_GetDuration(cinemaid); // milliseconds
native Cinema_GetTitle(cinemaid, title[], size = sizeof title);
native Cinema_GetVideoId(cinemaid, videoid[], size = sizeof videoid);
// The page that shows the cinema: http://<public host>:<port>/cinema/<id>/
native Cinema_GetPageUrl(cinemaid, url[], size = sizeof url);

forward OnCinemaFilmStart(cinemaid, requestid, const title[], duration_ms);
forward OnCinemaFilmError(cinemaid, requestid, const query[], const error[]);
// The film played to its end and left the screen.
forward OnCinemaFilmEnd(cinemaid, requestid);

/*
 * Helpers
 */

// Tunes a player into a radio.  The optional position makes it a 3D sound
// (for example a boombox or a club speaker).
stock Radio_AddPlayer(playerid, radioid, Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false)
{
	new url[RADIO_URL_SIZE];
	if (!Radio_AttachPlayer(playerid, radioid, url, sizeof url))
	{
		return 0;
	}
	PlayAudioStreamForPlayer(playerid, url, posX, posY, posZ, distance, usepos);
	return 1;
}

stock Radio_RemovePlayer(playerid)
{
	if (!Radio_DetachPlayer(playerid))
	{
		return 0;
	}
	StopAudioStreamForPlayer(playerid);
	return 1;
}

// Formats milliseconds as m:ss.
stock Radio_FormatTime(milliseconds, output[], size = sizeof output)
{
	new seconds = milliseconds / 1000;
	format(output, size, "%d:%02d", seconds / 60, seconds % 60);
}

/*
 * Forgets a player's stream when they leave.  open.mp does this by itself;
 * SA-MP needs this hook.  Define AUDIOCAST_NO_HOOKS to disable it.
 */
#if !defined AUDIOCAST_NO_HOOKS
public OnPlayerDisconnect(playerid, reason)
{
	Radio_DetachPlayer(playerid);
	Radio_SetPlayerVolume(playerid, 100);
	#if defined ACast_OnPlayerDisconnect
		return ACast_OnPlayerDisconnect(playerid, reason);
	#else
		return 1;
	#endif
}
#if defined _ALS_OnPlayerDisconnect
	#undef OnPlayerDisconnect
#else
	#define _ALS_OnPlayerDisconnect
#endif
#define OnPlayerDisconnect ACast_OnPlayerDisconnect
#if defined ACast_OnPlayerDisconnect
	forward ACast_OnPlayerDisconnect(playerid, reason);
#endif
#endif
