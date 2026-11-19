/*
 *  AudioCast for open.mp and SA-MP
 *  Copyright (c) 2026 devbluen
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// A video for the cinemas, on disk in <data_dir>/cinema/<video id> as YouTube
// sends it: video.webm (VP9, no sound) and audio.webm (Opus).  samp-cef's CEF
// and browsers play both as they are, so nothing is converted.  Never changes
// once the loader has published it.
struct Film
{
	std::string videoId;
	std::string title; // UTF-8
	std::uint32_t durationMs = 0;
	std::filesystem::path directory;

	std::filesystem::path video() const { return directory / "video.webm"; }
	std::filesystem::path audio() const { return directory / "audio.webm"; }
};
