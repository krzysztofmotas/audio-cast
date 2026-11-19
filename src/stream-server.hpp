/*
 *  AudioCast for open.mp and SA-MP
 *  Copyright (c) 2026 devbluen
 */

#pragma once

#include "config.hpp"
#include "film.hpp"
#include "track-events.hpp"
#include "station.hpp"
#include <asio.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

class StreamSession;

// A Range header ("bytes=...") of a file of `size` bytes as [first, last]; partial when it names one.  False when it
// asks for nothing the file has (416).
bool ParseByteRange(const std::string& header, std::uint64_t size, std::uint64_t& first, std::uint64_t& last, bool& partial);

// Shoutcast/Icecast-style HTTP server.  Each listener is one HTTP response
// that never ends: the radio's frames are written to it as they are produced.
// All socket work runs on one network thread.
class StreamServer
{
	friend class StreamSession;

public:
	struct Ticket
	{
		int playerId = -1;
		int radioId = 0;
		int volume = 100; // percent, on top of the radio's volume
	};

	// What a cinema shows, as of `at`: its page polls it (/cinema/<id>/state)
	// and plays the film from there.
	struct CinemaState
	{
		std::string name;
		std::shared_ptr<const Film> film; // null: nothing on the screen
		std::uint32_t positionMs = 0;
		bool paused = false;
		bool loading = false;
		Station::Clock::time_point at;
	};

private:
	struct Slot
	{
		std::shared_ptr<Station> station;
		std::set<std::shared_ptr<StreamSession>> sessions;
	};

	AudioCastConfig config_;
	EventQueue& events_;
	std::unique_ptr<asio::io_context> io_;
	std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
	std::unique_ptr<asio::steady_timer> pumpTimer_;
	std::thread thread_;
	bool running_ = false;
	int boundPort_ = 0;

	// Network thread only.
	std::map<int, Slot> slots_;
	std::size_t sessionCount_ = 0;

	// Shared with the server thread.
	mutable std::mutex sharedMutex_;
	std::unordered_map<std::string, Ticket> tickets_;
	std::map<int, int> listenerCounts_;
	std::map<int, int> radioVolumes_; // percent; missing = 100
	std::unordered_map<std::string, int> connectedTickets_;
	std::map<int, CinemaState> cinemas_;

	void accept();
	void schedulePump();
	void pump();
	// Called by sessions once their request is read.
	void openStream(const std::shared_ptr<StreamSession>& session, int radioId, const std::string& token, bool isHead, bool wantsMetadata);
	void sessionClosed(StreamSession& session);
	// /cinema/<id>/...: the page, its state and the film's files.
	void openCinema(const std::shared_ptr<StreamSession>& session, const std::string& path, bool isHead, const std::string& range);
	// sharedMutex_ must be held.
	int volumeStepsLocked(int radioId, const std::string& token) const;

public:
	explicit StreamServer(EventQueue& events);
	~StreamServer();

	bool start(const AudioCastConfig& config, std::string& error);
	void stop();
	bool running() const { return running_; }
	// The listening port (useful when audiocast_port is 0), or 0.
	int boundPort() const { return boundPort_; }

	// Server thread.
	void addStation(std::shared_ptr<Station> station);
	void removeStation(int radioId);
	void addTicket(const std::string& token, Ticket ticket);
	void removeTicket(const std::string& token);
	int listenerCount(int radioId) const;
	// Volumes in percent (0-100).  They apply to the audio sent from now
	// on, so listeners hear the change after the burst they already hold.
	void setRadioVolume(int radioId, int percent);
	void setTicketVolume(const std::string& token, int percent);
	bool ticketConnected(const std::string& token) const;
	void setCinema(int cinemaId, CinemaState state);
	void removeCinema(int cinemaId);
};
