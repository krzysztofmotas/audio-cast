/*
 *  AudioCast for open.mp and SA-MP
 *  Copyright (c) 2026 devbluen
 */

#include "stream-server.hpp"
#include "log.hpp"
#include "mp3.hpp"
#include "text.hpp"
#include "version.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <fstream>
#include <sstream>
#include <vector>

using tcp = asio::ip::tcp;

namespace
{
constexpr auto PUMP_INTERVAL = std::chrono::milliseconds(20);
constexpr auto REQUEST_TIMEOUT = std::chrono::seconds(10);
constexpr std::size_t MAX_REQUEST_SIZE = 8192;
constexpr std::size_t ICY_METADATA_INTERVAL = 16000;

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string headerSafe(const std::string& text, int codepage)
{
	std::string safe = Text::utf8ToCodepage(text, codepage);
	safe.erase(std::remove_if(safe.begin(), safe.end(), [](char c) { return c == '\r' || c == '\n'; }), safe.end());
	if (safe.size() > 200) safe.resize(200);
	return safe;
}

// The audio at a volume (MP3 frames with lowered global gain).
std::string attenuated(std::string audio, int steps)
{
	Mp3::attenuate(reinterpret_cast<std::uint8_t*>(audio.data()), audio.size(), steps);
	return audio;
}

std::string plainResponse(const char* status)
{
	return std::string("HTTP/1.0 ") + status + "\r\nContent-Type: text/plain\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n" + status + "\n";
}
}

namespace
{
// A file is sent in pieces of this size as the client takes them.
constexpr std::size_t FILE_CHUNK = 64 * 1024;

// Shows a cinema's film (/cinema/<id>/): plays its picture and sound in step with the cinema's clock, which it
// reads from ./state.  samp-cef draws the page on an object; a browser-based client takes the <video> from it.
//
// With ?canvas=<width>x<height> the page draws the picture on a transparent canvas of that many pixels instead (the
// texture a browser-based client makes), so a screen model larger than the screen it stands for shows nothing
// outside it: area=<w>,<h> is the centred part of the canvas the film fills (fractions, default the whole canvas),
// fit=contain|cover|fill how it fills it, brightness=<factor> lightens a screen the game's lighting darkens
// (webbrightness=<factor> instead in a browser-based client).
const char* const kCinemaPage = R"AUDIOCAST_PAGE(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>AudioCast cinema</title>
@SCRIPT@<style>
html, body { margin: 0; width: 100%; height: 100%; background: #000; overflow: hidden; }
html.drawn, html.drawn body { background: transparent; }
video { display: block; width: 100%; height: 100%; object-fit: contain; background: #000; }
html.drawn video { position: absolute; width: 1px; height: 1px; opacity: 0; }
canvas { display: none; }
html.drawn canvas { display: block; width: 100%; height: 100%; }
</style>
</head>
<body>
<video id="picture" muted playsinline preload="auto"></video>
<canvas id="screen" data-cef-surface></canvas>
<audio id="sound" preload="auto"></audio>
<script>
"use strict";
(() => {
  const picture = document.getElementById("picture");
  const sound = document.getElementById("sound");

  const options = new URLSearchParams(location.search);
  const canvasSize = /^(\d{1,4})x(\d{1,4})$/.exec(options.get("canvas") || "");
  const screen = document.getElementById("screen");
  if (canvasSize) {
    document.documentElement.classList.add("drawn");
    screen.width = Number(canvasSize[1]);
    screen.height = Number(canvasSize[2]);
    const fraction = (text) => Math.min(Math.max(Number(text) || 1, 0.01), 1);
    const [areaWidth, areaHeight] = (options.get("area") || "1,1").split(",").map(fraction);
    const fit = options.get("fit") || "contain";
    // A browser-based client shows the page in a frame of its own page and draws the screen darker than samp-cef
    const framed = window.parent !== window && options.has("webbrightness");
    const brightness = Math.min(Math.max(Number(options.get(framed ? "webbrightness" : "brightness")) || 1, 0.1), 4);
    const context = screen.getContext("2d");
    const area = { width: screen.width * areaWidth, height: screen.height * areaHeight };
    area.x = (screen.width - area.width) / 2;
    area.y = (screen.height - area.height) / 2;
    const draw = () => {
      context.clearRect(0, 0, screen.width, screen.height);
      context.fillStyle = "#000";
      context.fillRect(area.x, area.y, area.width, area.height);
      if (picture.readyState < 2 || !picture.videoWidth) return;
      const film = picture.videoWidth / picture.videoHeight;
      const target = area.width / area.height;
      let width = area.width, height = area.height;
      if (fit === "contain") film > target ? height = width / film : width = height * film;
      else if (fit === "cover") film > target ? width = height * film : height = width / film;
      context.save();
      context.beginPath();
      context.rect(area.x, area.y, area.width, area.height);
      context.clip();
      if (brightness !== 1) context.filter = `brightness(${brightness})`;
      context.drawImage(picture, area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height);
      context.restore();
    };
    const onFrame = () => {
      draw();
      picture.requestVideoFrameCallback(onFrame);
    };
    if (picture.requestVideoFrameCallback) picture.requestVideoFrameCallback(onFrame);
    else setInterval(draw, 33);
    for (const event of ["loadeddata", "seeked", "emptied"]) picture.addEventListener(event, draw);
    draw();
  }
  // How far the sound may get from the cinema's clock, and the picture from the sound, before a seek (seconds)
  const SOUND_DRIFT = 0.75;
  const PICTURE_DRIFT = 0.15;
  const POLL_MS = 1000;
  let filmId = null;
  let clock = null; // { position (s), paused, at (performance.now()) }
  const position = () => clock.position + (clock.paused ? 0 : (performance.now() - clock.at) / 1000);

  const show = (film) => {
    const id = film ? film.id : null;
    if (id === filmId) return;
    filmId = id;
    if (!id) {
      for (const media of [picture, sound]) { media.removeAttribute("src"); media.load(); }
      return;
    }
    picture.src = "video.webm?v=" + encodeURIComponent(id);
    sound.src = "audio.webm?v=" + encodeURIComponent(id);
  };
  // A browser that wants a click before sound plays the film silent rather than not at all
  const play = (media) => media.play().catch(() => {
    if (media === sound && !sound.muted) {
      sound.muted = true;
      sound.play().catch(() => {});
    }
  });
  const keepUp = () => {
    if (!filmId || !clock) return;
    const wanted = position();
    if (sound.readyState >= 1 && Math.abs(sound.currentTime - wanted) > SOUND_DRIFT) sound.currentTime = wanted;
    if (picture.readyState >= 1 && Math.abs(picture.currentTime - sound.currentTime) > PICTURE_DRIFT) picture.currentTime = sound.currentTime;
    for (const media of [sound, picture]) {
      if (clock.paused && !media.paused) media.pause();
      else if (!clock.paused && media.paused) play(media);
    }
  };
  const poll = async () => {
    try {
      const sent = performance.now();
      const state = await (await fetch("state", { cache: "no-store" })).json();
      const received = performance.now();
      show(state.film);
      // the state left the server about half the round trip ago
      const travelled = state.paused ? 0 : (received - sent) / 2000;
      clock = { position: state.position / 1000 + travelled, paused: state.paused, at: received };
      keepUp();
    } catch (e) {
    }
    setTimeout(poll, POLL_MS);
  };
  setInterval(keepUp, 250);
  poll();
})();
</script>
</body>
</html>
)AUDIOCAST_PAGE";

// Headers of every cinema response: the page sits in a frame of another site (a browser-based client, whose page is
// cross-origin isolated) and reads its files from there.
const char* const kCinemaHeaders =
	"Access-Control-Allow-Origin: *\r\n"
	"Cross-Origin-Resource-Policy: cross-origin\r\n"
	"Connection: close\r\n"
	"Server: AudioCast/" AUDIOCAST_VERSION "\r\n";

std::string jsonString(const std::string& text)
{
	std::string json = "\"";
	for (const unsigned char c : text)
	{
		if (c == '"' || c == '\\')
		{
			json += '\\';
			json += static_cast<char>(c);
		}
		else if (c < 0x20)
		{
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
			json += escaped;
		}
		else json += static_cast<char>(c);
	}
	return json + "\"";
}

std::string htmlAttribute(const std::string& text)
{
	std::string escaped;
	for (const char c : text)
	{
		if (c == '"') escaped += "&quot;";
		else if (c == '&') escaped += "&amp;";
		else if (c == '<') escaped += "&lt;";
		else escaped += c;
	}
	return escaped;
}
}

// "bytes=<first>-<last>", "bytes=<first>-" or "bytes=-<suffix>" of a file of `size` bytes.  False when it asks for
// nothing the file has (416); no header, or one this does not handle (several ranges), is the whole file.
bool ParseByteRange(const std::string& header, std::uint64_t size, std::uint64_t& first, std::uint64_t& last, bool& partial)
{
	partial = false;
	first = 0;
	last = size == 0 ? 0 : size - 1;
	if (header.compare(0, 6, "bytes=") != 0 || header.find(',') != std::string::npos) return true;
	const std::string spec = header.substr(6);
	const std::size_t dash = spec.find('-');
	if (dash == std::string::npos) return true;
	if (size == 0) return false;
	try
	{
		if (dash == 0)
		{
			const std::uint64_t suffix = std::stoull(spec.substr(1));
			if (suffix == 0) return false;
			first = suffix >= size ? 0 : size - suffix;
		}
		else
		{
			first = std::stoull(spec.substr(0, dash));
			if (dash + 1 < spec.size()) last = std::min<std::uint64_t>(std::stoull(spec.substr(dash + 1)), size - 1);
		}
	}
	catch (...)
	{
		return true;
	}
	if (first >= size || first > last) return false;
	partial = true;
	return true;
}

class StreamSession : public std::enable_shared_from_this<StreamSession>
{
private:
	StreamServer& server_;
	tcp::socket socket_;
	asio::streambuf request_;
	asio::steady_timer timeout_;
	std::deque<std::string> outbox_;
	std::size_t queuedBytes_ = 0;
	std::size_t maxQueuedBytes_ = 0;
	bool writing_ = false;
	bool closeAfterWrite_ = false;
	bool closed_ = false;
	bool icy_ = false;
	// A file being sent (a cinema's film), read a piece at a time as the client takes them.
	std::ifstream file_;
	std::uint64_t fileLeft_ = 0;
	std::size_t untilMetadata_ = ICY_METADATA_INTERVAL;
	std::string sentTitle_;

	void onRequest(const std::error_code& error)
	{
		if (closed_) return;
		if (error)
		{
			close();
			return;
		}

		std::istream stream(&request_);
		std::string requestLine;
		std::getline(stream, requestLine);
		bool wantsMetadata = false;
		std::string range;
		std::string header;
		while (std::getline(stream, header) && header != "\r" && !header.empty())
		{
			const std::string lowered = lower(header);
			if (lowered.rfind("icy-metadata:", 0) == 0 && lowered.find('1') != std::string::npos) wantsMetadata = true;
			if (lowered.rfind("range:", 0) == 0)
			{
				range = lowered.substr(6);
				range.erase(std::remove_if(range.begin(), range.end(), [](unsigned char c) { return std::isspace(c); }), range.end());
			}
		}

		// "GET /radio/<id>/<token>.mp3 HTTP/1.1"
		std::istringstream line(requestLine);
		std::string method;
		std::string target;
		line >> method >> target;
		const bool isHead = method == "HEAD";
		if ((method == "GET" || isHead) && target.compare(0, 8, "/cinema/") == 0)
		{
			std::string path = target.substr(8);
			if (const std::size_t query = path.find_first_of("?#"); query != std::string::npos) path.resize(query);
			server_.openCinema(shared_from_this(), path, isHead, range);
			return;
		}
		if ((method != "GET" && !isHead) || target.compare(0, 7, "/radio/") != 0)
		{
			reply(plainResponse("404 Not Found"));
			return;
		}

		std::string path = target.substr(7);
		if (const std::size_t query = path.find_first_of("?#"); query != std::string::npos) path.resize(query);
		if (path.size() > 4 && lower(path.substr(path.size() - 4)) == ".mp3") path.resize(path.size() - 4);
		std::string idText = path;
		std::string requestToken;
		if (const std::size_t slash = path.find('/'); slash != std::string::npos)
		{
			idText = path.substr(0, slash);
			requestToken = path.substr(slash + 1);
		}

		if (idText.empty() || idText.size() > 9 || !std::all_of(idText.begin(), idText.end(), [](unsigned char c) { return std::isdigit(c); }))
		{
			reply(plainResponse("404 Not Found"));
			return;
		}
		server_.openStream(shared_from_this(), std::stoi(idText), requestToken, isHead, wantsMetadata);
	}

	// The next piece of the file being sent, once the client has taken the last one.
	void readFile()
	{
		if (fileLeft_ == 0 || !outbox_.empty()) return;
		std::string piece(static_cast<std::size_t>(std::min<std::uint64_t>(fileLeft_, FILE_CHUNK)), '\0');
		if (!file_.read(piece.data(), static_cast<std::streamsize>(piece.size())))
		{
			fileLeft_ = 0;
			return;
		}
		fileLeft_ -= piece.size();
		queuedBytes_ += piece.size();
		outbox_.push_back(std::move(piece));
	}

	void write()
	{
		if (!closed_) readFile();
		if (closed_ || outbox_.empty())
		{
			writing_ = false;
			if (closeAfterWrite_) close();
			return;
		}
		writing_ = true;
		auto self = shared_from_this();
		asio::async_write(socket_, asio::buffer(outbox_.front()), [self](const std::error_code& error, std::size_t)
		{
			if (self->closed_) return;
			if (error)
			{
				self->close();
				return;
			}
			self->queuedBytes_ -= self->outbox_.front().size();
			self->outbox_.pop_front();
			self->write();
		});
	}

	std::string metadataBlock(const std::string& title)
	{
		std::string text = Text::utf8ToCodepage(title, server_.config_.metadataCodepage);
		if (text == sentTitle_) return std::string(1, '\0');
		sentTitle_ = text;
		std::replace(text.begin(), text.end(), '\'', '`');
		text = "StreamTitle='" + text + "';";
		if (text.size() > 255 * 16) text.resize(255 * 16);
		const std::size_t blocks = (text.size() + 15) / 16;
		text.resize(blocks * 16, '\0');
		return static_cast<char>(blocks) + text;
	}

public:
	int radioId = 0;
	int playerId = -1;
	std::string token;
	bool attached = false;

	StreamSession(StreamServer& server, tcp::socket socket, std::size_t maxQueuedBytes)
		: server_(server)
		, socket_(std::move(socket))
		, request_(MAX_REQUEST_SIZE)
		, timeout_(socket_.get_executor())
		, maxQueuedBytes_(maxQueuedBytes)
	{
	}

	void start()
	{
		std::error_code ignored;
		socket_.set_option(tcp::no_delay(true), ignored);

		auto self = shared_from_this();
		timeout_.expires_after(REQUEST_TIMEOUT);
		timeout_.async_wait([self](const std::error_code& error)
		{
			if (!error && !self->attached && !self->closed_) self->close();
		});
		asio::async_read_until(socket_, request_, "\r\n\r\n", [self](const std::error_code& error, std::size_t)
		{
			self->onRequest(error);
		});
	}

	void enableMetadata() { icy_ = true; }

	// Sends `headers`, then `length` bytes of `path` from `offset`, and closes the connection.
	void sendFile(std::string headers, const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length)
	{
		file_.open(path, std::ios::binary);
		if (!file_ || !file_.seekg(static_cast<std::streamoff>(offset)))
		{
			reply(plainResponse("404 Not Found"));
			return;
		}
		timeout_.cancel(); // the request timeout ends here; a film downloads for as long as it takes
		fileLeft_ = length;
		closeAfterWrite_ = true;
		push(std::move(headers));
	}

	// Sends a complete response and closes the connection.
	void reply(std::string response)
	{
		push(std::move(response));
		closeAfterWrite_ = true;
		if (!writing_) write();
	}

	void push(std::string data)
	{
		if (closed_ || data.empty()) return;
		// A client that cannot keep up (or stopped reading) is dropped instead
		// of buffering audio forever.
		if (queuedBytes_ + data.size() > maxQueuedBytes_)
		{
			close();
			return;
		}
		queuedBytes_ += data.size();
		outbox_.push_back(std::move(data));
		if (!writing_) write();
	}

	void sendAudio(const std::string& audio, const std::string& title)
	{
		if (closed_ || audio.empty()) return;
		if (!icy_)
		{
			push(audio);
			return;
		}

		// Shoutcast metadata: a title block after every ICY_METADATA_INTERVAL
		// bytes of audio.
		std::string chunk;
		chunk.reserve(audio.size() + 64);
		std::size_t position = 0;
		while (position < audio.size())
		{
			const std::size_t count = std::min(untilMetadata_, audio.size() - position);
			chunk.append(audio, position, count);
			position += count;
			untilMetadata_ -= count;
			if (untilMetadata_ == 0)
			{
				chunk += metadataBlock(title);
				untilMetadata_ = ICY_METADATA_INTERVAL;
			}
		}
		push(std::move(chunk));
	}

	void close()
	{
		if (closed_) return;
		closed_ = true;
		auto self = shared_from_this();
		std::error_code ignored;
		timeout_.cancel();
		socket_.shutdown(tcp::socket::shutdown_both, ignored);
		socket_.close(ignored);
		outbox_.clear();
		queuedBytes_ = 0;
		server_.sessionClosed(*this);
	}
};

StreamServer::StreamServer(EventQueue& events)
	: events_(events)
{
}

StreamServer::~StreamServer()
{
	stop();
}

bool StreamServer::start(const AudioCastConfig& config, std::string& error)
{
	if (running_) return true;
	config_ = config;
	io_ = std::make_unique<asio::io_context>(1);
	pumpTimer_ = std::make_unique<asio::steady_timer>(*io_);

	// The radios keep their clock even when the port cannot be opened, so
	// the network thread always runs.
	bool listening = false;
	try
	{
		const tcp::endpoint endpoint(asio::ip::make_address(config_.bindAddress), static_cast<unsigned short>(config_.port));
		acceptor_ = std::make_unique<tcp::acceptor>(*io_);
		acceptor_->open(endpoint.protocol());
		acceptor_->set_option(tcp::acceptor::reuse_address(true));
		acceptor_->bind(endpoint);
		acceptor_->listen();
		boundPort_ = acceptor_->local_endpoint().port();
		listening = true;
	}
	catch (const std::exception& exception)
	{
		error = "could not listen on " + config_.bindAddress + ":" + std::to_string(config_.port) + " - " + exception.what();
		acceptor_.reset();
	}

	if (acceptor_) accept();
	schedulePump();
	running_ = true;
	thread_ = std::thread([this]
	{
		for (;;)
		{
			try
			{
				io_->run();
				return;
			}
			catch (const std::exception& exception)
			{
				AudioCastLogWarning(std::string("network thread error: ") + exception.what());
			}
		}
	});
	return listening;
}

void StreamServer::stop()
{
	if (!running_) return;
	asio::post(*io_, [this]
	{
		std::error_code ignored;
		if (acceptor_) acceptor_->close(ignored);
		pumpTimer_->cancel();
		for (auto& entry : slots_)
		{
			const auto sessions = entry.second.sessions;
			for (const auto& session : sessions) session->close();
		}
		slots_.clear();
		io_->stop();
	});
	if (thread_.joinable()) thread_.join();
	slots_.clear();
	pumpTimer_.reset();
	acceptor_.reset();
	io_.reset();
	running_ = false;
	boundPort_ = 0;

	std::lock_guard<std::mutex> lock(sharedMutex_);
	tickets_.clear();
	listenerCounts_.clear();
	radioVolumes_.clear();
	connectedTickets_.clear();
	cinemas_.clear();
	sessionCount_ = 0;
}

void StreamServer::accept()
{
	acceptor_->async_accept([this](const std::error_code& error, tcp::socket socket)
	{
		if (!acceptor_ || !acceptor_->is_open()) return;
		if (!error)
		{
			// Room for 30 seconds of audio plus the burst.
			const std::size_t maxQueued = static_cast<std::size_t>(config_.bitrateKbps) * 125u *
				(30u + static_cast<std::size_t>(config_.burstMs) / 1000u) + 65536u;
			std::make_shared<StreamSession>(*this, std::move(socket), maxQueued)->start();
		}
		accept();
	});
}

void StreamServer::schedulePump()
{
	pumpTimer_->expires_after(PUMP_INTERVAL);
	pumpTimer_->async_wait([this](const std::error_code& error)
	{
		if (error) return;
		pump();
		schedulePump();
	});
}

void StreamServer::pump()
{
	const auto now = Station::Clock::now();
	std::string audio;
	for (auto& entry : slots_)
	{
		audio.clear();
		entry.second.station->produce(now, audio);
		if (audio.empty() || entry.second.sessions.empty()) continue;

		const std::string title = entry.second.station->streamTitle();
		const std::vector<std::shared_ptr<StreamSession>> sessions(entry.second.sessions.begin(), entry.second.sessions.end());
		std::vector<int> steps;
		{
			std::lock_guard<std::mutex> lock(sharedMutex_);
			for (const auto& session : sessions) steps.push_back(volumeStepsLocked(entry.first, session->token));
		}
		// One copy per volume in use; listeners at full volume share the original.
		std::map<int, std::string> versions;
		for (std::size_t index = 0; index < sessions.size(); ++index)
		{
			if (steps[index] == 0)
			{
				sessions[index]->sendAudio(audio, title);
				continue;
			}
			auto version = versions.find(steps[index]);
			if (version == versions.end()) version = versions.emplace(steps[index], attenuated(audio, steps[index])).first;
			sessions[index]->sendAudio(version->second, title);
		}
	}
}

void StreamServer::openStream(const std::shared_ptr<StreamSession>& session, int radioId, const std::string& token, bool isHead,
	bool wantsMetadata)
{
	int playerId = -1;
	if (token.empty())
	{
		if (!config_.allowPublic)
		{
			session->reply(plainResponse("403 Forbidden"));
			return;
		}
	}
	else
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		const auto ticket = tickets_.find(token);
		if (ticket == tickets_.end() || ticket->second.radioId != radioId)
		{
			session->reply(plainResponse("403 Forbidden"));
			return;
		}
		playerId = ticket->second.playerId;
	}
	int steps = 0;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		steps = volumeStepsLocked(radioId, token);
	}

	const auto slot = slots_.find(radioId);
	if (slot == slots_.end())
	{
		session->reply(plainResponse("404 Not Found"));
		return;
	}

	std::string headers = "HTTP/1.0 200 OK\r\n"
		"Content-Type: audio/mpeg\r\n"
		"Cache-Control: no-cache, no-store\r\n"
		"Pragma: no-cache\r\n"
		"Connection: close\r\n"
		"Server: AudioCast/" AUDIOCAST_VERSION "\r\n"
		"icy-name: " + headerSafe(slot->second.station->name(), config_.metadataCodepage) + "\r\n"
		"icy-br: " + std::to_string(config_.bitrateKbps) + "\r\n"
		"icy-pub: 0\r\n";
	if (wantsMetadata) headers += "icy-metaint: " + std::to_string(ICY_METADATA_INTERVAL) + "\r\n";
	headers += "\r\n";

	if (isHead)
	{
		session->reply(std::move(headers));
		return;
	}

	if (sessionCount_ >= static_cast<std::size_t>(config_.maxListeners))
	{
		session->reply(plainResponse("503 Service Unavailable"));
		return;
	}

	// The same player opening the stream again (a new PlayAudioStream)
	// replaces the old connection.
	if (!token.empty())
	{
		std::vector<std::shared_ptr<StreamSession>> previous;
		for (auto& entry : slots_)
		{
			for (const auto& other : entry.second.sessions)
			{
				if (other->token == token) previous.push_back(other);
			}
		}
		for (const auto& other : previous) other->close();
	}

	session->attached = true;
	session->radioId = radioId;
	session->playerId = playerId;
	session->token = token;
	slot->second.sessions.insert(session);
	++sessionCount_;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		++listenerCounts_[radioId];
		if (!token.empty()) ++connectedTickets_[token];
	}

	AudioCastEvent event;
	event.type = AudioCastEvent::Type::ListenerConnect;
	event.radioId = radioId;
	event.playerId = playerId;
	events_.push(std::move(event));

	if (wantsMetadata) session->enableMetadata();
	session->push(std::move(headers));
	// Every listener starts with the same amount of recent audio, so all of
	// them play the same position of the song.
	session->sendAudio(attenuated(slot->second.station->burst(), steps), slot->second.station->streamTitle());
}

int StreamServer::volumeStepsLocked(int radioId, const std::string& token) const
{
	double percent = 100.0;
	const auto radio = radioVolumes_.find(radioId);
	if (radio != radioVolumes_.end()) percent = radio->second;
	if (!token.empty())
	{
		const auto ticket = tickets_.find(token);
		if (ticket != tickets_.end()) percent = percent * ticket->second.volume / 100.0;
	}
	return Mp3::volumeToSteps(percent);
}

void StreamServer::sessionClosed(StreamSession& session)
{
	if (!session.attached) return;
	session.attached = false;

	const auto slot = slots_.find(session.radioId);
	if (slot != slots_.end())
	{
		for (auto it = slot->second.sessions.begin(); it != slot->second.sessions.end(); ++it)
		{
			if (it->get() == &session)
			{
				slot->second.sessions.erase(it);
				break;
			}
		}
	}
	if (sessionCount_ > 0) --sessionCount_;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		if (--listenerCounts_[session.radioId] <= 0) listenerCounts_.erase(session.radioId);
		if (!session.token.empty())
		{
			const auto connected = connectedTickets_.find(session.token);
			if (connected != connectedTickets_.end() && --connected->second <= 0) connectedTickets_.erase(connected);
		}
	}

	AudioCastEvent event;
	event.type = AudioCastEvent::Type::ListenerDisconnect;
	event.radioId = session.radioId;
	event.playerId = session.playerId;
	events_.push(std::move(event));
}

void StreamServer::addStation(std::shared_ptr<Station> station)
{
	if (!running_ || !station) return;
	asio::post(*io_, [this, station]
	{
		slots_[station->id()].station = station;
	});
}

void StreamServer::removeStation(int radioId)
{
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		radioVolumes_.erase(radioId);
	}
	if (!running_) return;
	asio::post(*io_, [this, radioId]
	{
		const auto slot = slots_.find(radioId);
		if (slot == slots_.end()) return;
		const auto sessions = slot->second.sessions;
		for (const auto& session : sessions) session->close();
		slots_.erase(radioId);
	});
}

void StreamServer::addTicket(const std::string& token, Ticket ticket)
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	tickets_[token] = ticket;
}

void StreamServer::removeTicket(const std::string& token)
{
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		tickets_.erase(token);
	}
	if (!running_) return;
	asio::post(*io_, [this, token]
	{
		std::vector<std::shared_ptr<StreamSession>> sessions;
		for (auto& entry : slots_)
		{
			for (const auto& session : entry.second.sessions)
			{
				if (session->token == token) sessions.push_back(session);
			}
		}
		for (const auto& session : sessions) session->close();
	});
}

void StreamServer::setRadioVolume(int radioId, int percent)
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	if (percent >= 100) radioVolumes_.erase(radioId);
	else radioVolumes_[radioId] = std::max(percent, 0);
}

void StreamServer::setTicketVolume(const std::string& token, int percent)
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	const auto ticket = tickets_.find(token);
	if (ticket != tickets_.end()) ticket->second.volume = std::clamp(percent, 0, 100);
}

int StreamServer::listenerCount(int radioId) const
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	const auto it = listenerCounts_.find(radioId);
	return it == listenerCounts_.end() ? 0 : it->second;
}

bool StreamServer::ticketConnected(const std::string& token) const
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	return connectedTickets_.count(token) != 0;
}

void StreamServer::setCinema(int cinemaId, CinemaState state)
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	cinemas_[cinemaId] = std::move(state);
}

void StreamServer::removeCinema(int cinemaId)
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	cinemas_.erase(cinemaId);
}

void StreamServer::openCinema(const std::shared_ptr<StreamSession>& session, const std::string& path, bool isHead,
	const std::string& range)
{
	// "<id>", "<id>/", "<id>/state", "<id>/video.webm", "<id>/audio.webm"
	const std::size_t slash = path.find('/');
	const std::string idText = path.substr(0, slash);
	const std::string resource = slash == std::string::npos ? std::string() : path.substr(slash + 1);
	if (idText.empty() || idText.size() > 9 || !std::all_of(idText.begin(), idText.end(), [](unsigned char c) { return std::isdigit(c); }))
	{
		session->reply(plainResponse("404 Not Found"));
		return;
	}
	const int cinemaId = std::stoi(idText);

	CinemaState state;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		const auto cinema = cinemas_.find(cinemaId);
		if (cinema == cinemas_.end())
		{
			session->reply(plainResponse("404 Not Found"));
			return;
		}
		state = cinema->second;
	}

	// The page's own address ends in a slash, so "state" and the files resolve under it
	if (slash == std::string::npos)
	{
		session->reply(std::string("HTTP/1.0 301 Moved Permanently\r\nLocation: /cinema/") + idText + "/\r\n" + kCinemaHeaders + "\r\n");
		return;
	}

	if (resource.empty() || resource == "index.html")
	{
		std::string page = kCinemaPage;
		const std::string script = config_.cinemaScript.empty()
			? std::string()
			: "<script src=\"" + htmlAttribute(config_.cinemaScript) + "\"></script>\n";
		page.replace(page.find("@SCRIPT@"), 8, script);
		std::string response = std::string("HTTP/1.0 200 OK\r\n"
			"Content-Type: text/html; charset=utf-8\r\n"
			"Cache-Control: no-cache\r\n"
			// so a cross-origin isolated page may frame it in every browser
			"Cross-Origin-Embedder-Policy: require-corp\r\n") +
			kCinemaHeaders + "Content-Length: " + std::to_string(page.size()) + "\r\n\r\n";
		if (!isHead) response += page;
		session->reply(std::move(response));
		return;
	}

	if (resource == "state")
	{
		std::uint32_t position = state.positionMs;
		if (state.film && !state.paused)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Station::Clock::now() - state.at).count();
			position += static_cast<std::uint32_t>(std::max<long long>(elapsed, 0));
			if (state.film->durationMs > 0) position = std::min(position, state.film->durationMs);
		}
		std::string json = "{\"name\":" + jsonString(state.name) + ",\"film\":";
		if (state.film)
		{
			json += "{\"id\":" + jsonString(state.film->videoId) + ",\"title\":" + jsonString(state.film->title) +
				",\"duration\":" + std::to_string(state.film->durationMs) + "}";
		}
		else json += "null";
		json += ",\"position\":" + std::to_string(position) + ",\"paused\":" + (state.paused ? "true" : "false") +
			",\"loading\":" + (state.loading ? "true" : "false") + "}";
		std::string response = std::string("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n") +
			kCinemaHeaders + "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n";
		if (!isHead) response += json;
		session->reply(std::move(response));
		return;
	}

	const bool video = resource.compare(0, 10, "video.webm") == 0;
	const bool audio = resource.compare(0, 10, "audio.webm") == 0;
	if ((!video && !audio) || !state.film)
	{
		session->reply(plainResponse("404 Not Found"));
		return;
	}
	const std::filesystem::path file = video ? state.film->video() : state.film->audio();
	std::error_code error;
	const std::uint64_t size = std::filesystem::file_size(file, error);
	if (error)
	{
		session->reply(plainResponse("404 Not Found"));
		return;
	}
	std::uint64_t first = 0;
	std::uint64_t last = 0;
	bool partial = false;
	if (!ParseByteRange(range, size, first, last, partial))
	{
		session->reply(std::string("HTTP/1.0 416 Range Not Satisfiable\r\nContent-Range: bytes */") + std::to_string(size) + "\r\n" +
			kCinemaHeaders + "\r\n");
		return;
	}
	const std::uint64_t length = size == 0 ? 0 : last - first + 1;
	std::string headers = std::string("HTTP/1.0 ") + (partial ? "206 Partial Content" : "200 OK") + "\r\n" +
		"Content-Type: " + (video ? "video/webm" : "audio/webm") + "\r\n"
		"Accept-Ranges: bytes\r\n"
		"Cache-Control: no-cache\r\n" +
		kCinemaHeaders + "Content-Length: " + std::to_string(length) + "\r\n";
	if (partial) headers += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size) + "\r\n";
	headers += "\r\n";
	if (isHead || length == 0)
	{
		session->reply(std::move(headers));
		return;
	}
	session->sendFile(std::move(headers), file, first, length);
}
