/*
 *  AudioCast for open.mp and SA-MP
 *  Copyright (c) 2026 devbluen
 */

#pragma once

#include "config.hpp"
#include "track-events.hpp"
#include "loader.hpp"
#include "samp-pawn.hpp"
#include "station.hpp"
#include "stream-server.hpp"
#include <Server/Components/Pawn/pawn.hpp>
#include <sdk.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

struct IAudioCastComponent : public IComponent
{
	PROVIDE_UID(0x4A75C1D3A0B7F296);
};

// One binary, two hosts: loaded from components/ it is an open.mp component,
// loaded from plugins/ it is a classic SA-MP plugin (see samp-main.cpp).
class AudioCastComponent final
	: public IAudioCastComponent
	, public CoreEventHandler
	, public PawnEventHandler
	, public PlayerConnectEventHandler
{
private:
	struct Radio
	{
		std::shared_ptr<Station> station;
		AMX* owner = nullptr;
		int volume = 100;
	};

	struct Listener
	{
		int radioId = 0;
		std::string token;
	};

	ICore* core_ = nullptr;
	IPawnComponent* pawn_ = nullptr;
	SampPawnComponent sampPawn_;
	bool sampMode_ = false;

	AudioCastConfig config_;
	bool started_ = false;
	std::string publicHost_;
	int publicPort_ = 0;

	EventQueue events_;
	TrackLoader loader_;
	StreamServer server_ { events_ };

	std::map<int, Radio> radios_;
	std::unordered_map<int, Listener> listeners_;
	// Players whose volume is not 100; kept across radios until they leave.
	std::unordered_map<int, int> playerVolumes_;

	struct Search
	{
		AMX* owner = nullptr;
		bool done = false;
		std::vector<SearchEntry> entries;
		std::string error;
	};
	// Kept until Radio_FreeSearch, the owner script unloads, or too many pile up.
	std::map<int, Search> searches_;
	int nextSearchId_ = 1;
	int nextRadioId_ = 1;
	int nextRequestId_ = 1;

public:
	// A screen showing one film to everyone at the same moment.  Its page
	// (/cinema/<id>/) follows the clock kept here.
	struct Cinema
	{
		std::string name; // UTF-8
		AMX* owner = nullptr;
		std::shared_ptr<const Film> film; // null: nothing on the screen
		int requestId = 0;                // of the film on the screen
		int loadingRequestId = 0;         // of the film Cinema_Play is fetching
		std::uint32_t positionMs = 0;     // as of `at`
		bool paused = false;
		Station::Clock::time_point at;

		std::uint32_t position(Station::Clock::time_point now) const;
	};

private:
	std::map<int, Cinema> cinemas_;
	int nextCinemaId_ = 1;
	std::set<std::string> filmsInUse_;

	static AudioCastComponent* instance_;

	std::string configValue(const std::string& key) const;
	void startServices();
	void stopServices();
	void destroyRadiosOwnedBy(AMX* amx);
	void tick();
	void deliverLoadResults();
	void deliverEvents();
	void deliverFilm(LoadResult& result);
	// Films that have played to their end leave the screen.
	void checkCinemas();
	// The cinema's state to its page (StreamServer), and the films in use to the loader's cache.
	void publishCinema(int cinemaId);

	template <typename... Args>
	void callPublic(const char* name, Args... args);

public:
	// IComponent
	StringView componentName() const override { return "AudioCast"; }
	SemanticVersion componentVersion() const override;
	void onLoad(ICore* core) override;
	void onInit(IComponentList* components) override;
	void onReady() override;
	void onFree(IComponent* component) override;
	void provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) override;
	void free() override;
	void reset() override;

	void onTick(Microseconds elapsed, TimePoint now) override;
	void onAmxLoad(IPawnScript& script) override;
	void onAmxUnload(IPawnScript& script) override;
	void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override;

	// SA-MP plugin entry points.
	void sampLoad();
	void sampUnload();
	void sampAmxLoad(AMX* amx);
	void sampAmxUnload(AMX* amx);
	void sampTick();

	// Used by the natives (server thread).
	IPawnScript* script(AMX* amx);
	const AudioCastConfig& config() const { return config_; }
	std::string toPawn(const std::string& utf8) const;
	std::string fromPawn(const std::string& text) const;

	int createRadio(AMX* owner, const std::string& name);
	bool destroyRadio(int radioId);
	std::shared_ptr<Station> radio(int radioId) const;
	int requestTrack(int radioId, const std::string& query, bool interrupt);
	int startSearch(AMX* owner, const std::string& query, int maxResults);
	const SearchEntry* searchEntry(int searchId, int index) const;
	int searchCount(int searchId) const;
	bool freeSearch(int searchId);
	bool attachPlayer(int playerId, int radioId, std::string& url);
	bool detachPlayer(int playerId);
	int playerRadio(int playerId) const;
	bool playerStreaming(int playerId) const;
	int listenerCount(int radioId) const;
	bool setRadioVolume(int radioId, int volume);
	int radioVolume(int radioId) const;
	bool setPlayerVolume(int playerId, int volume);
	int playerVolume(int playerId) const;
	std::string publicUrl(int radioId) const;

	int createCinema(AMX* owner, const std::string& name);
	bool destroyCinema(int cinemaId);
	const Cinema* cinema(int cinemaId) const;
	// Returns the request ID; the film replaces what is on the screen once it is on disk.
	int playFilm(int cinemaId, const std::string& query);
	bool stopCinema(int cinemaId);
	bool pauseCinema(int cinemaId, bool paused);
	bool seekCinema(int cinemaId, std::uint32_t positionMs);
	std::uint32_t cinemaPosition(int cinemaId) const;
	std::string cinemaPageUrl(int cinemaId) const;

	static AudioCastComponent* getInstance();
};
