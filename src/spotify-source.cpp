/*
OBS Now Playing Plugin
Copyright (C) 2026 lingeriegoat https://github.com/lingeriegoat

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "spotify-source.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/bmem.h>

#define NOMINMAX
#define GDIPVER 0x0110 // pull in GDI+ 1.1 (Blur/effects, Bitmap::ApplyEffect)
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <GdiplusEffects.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

constexpr int DEFAULT_SESSION_GRACE_SECONDS = 3;
constexpr int POLL_INTERVAL_MS = 250; // how often we poll SMTC
constexpr int DEFAULT_CARD_W = 380;
constexpr int DEFAULT_CARD_H = 100;
constexpr int PAD = 12;
constexpr int MIN_TEXT_W = 20;
constexpr auto SCROLL_END_PAUSE = std::chrono::seconds(2);
constexpr int MIN_ART_SIZE = 10;
constexpr int DEFAULT_BACKGROUND_CORNER_RADIUS = 14;
constexpr int DEFAULT_ALBUM_ART_CORNER_RADIUS = 8;
constexpr int DEFAULT_BG_OPACITY = 70;
constexpr int DEFAULT_SCROLL_SPEED_MS = 500;
constexpr int DEFAULT_TITLE_FONT_SIZE = 22;
constexpr int DEFAULT_TITLE_FONT_FLAGS = 0;
constexpr const char *DEFAULT_TITLE_FONT_FACE = "Segoe UI";
constexpr const char *DEFAULT_TITLE_FONT_STYLE = "Regular";
constexpr int DEFAULT_ARTIST_FONT_SIZE = 20;
constexpr int DEFAULT_ARTIST_FONT_FLAGS = 0;
constexpr const char *DEFAULT_ARTIST_FONT_FACE = "Segoe UI";
constexpr const char *DEFAULT_ARTIST_FONT_STYLE = "Regular";
constexpr int DEFAULT_COLOR_WHITE = 0xFFFFFFFF;
constexpr int DEFAULT_COLOR_BLACK = 0xFF000000;
constexpr int DEFAULT_COLOR_DARK_GREY = 0xFF5A5A5A;
constexpr int DEFAULT_COLOR_GREEN = 0xFF60D71E;
constexpr int DEFAULT_ALBUM_ART_BG_BLUR_PCT = 50;
constexpr int DEFAULT_TEXT_OUTLINE_SIZE_PX = 2;
constexpr int DEFAULT_ANIMATION_UPDATE_MS = 100;

constexpr int DEFAULT_VU_COLOR = 0xFFFFFFFF;
constexpr int DEFAULT_VU_UPDATE_SPEED_MS = 250;
constexpr int DEFAULT_VU_RANDOMNESS = 50;
constexpr int DEFAULT_VU_WIDTH = 37;
constexpr int DEFAULT_VU_HEIGHT = 43;
constexpr int DEFAULT_VU_BAR_COUNT = 5;

constexpr int DEFAULT_VHS_INTENSITY = 50;
constexpr int DEFAULT_VHS_CHROMA_ABERRATION = 10;
constexpr int VHS_NOISE_TEXTURE_SIZE = 64;
constexpr int DEFAULT_VHS_SCANLINE_SPACING_PX = 3;
constexpr int DEFAULT_VHS_SCANLINE_INTENSITY = 50; // 0..100
constexpr int DEFAULT_VHS_TRACKING_MIN_INTERVAL_S = 6;
constexpr int DEFAULT_VHS_TRACKING_MAX_INTERVAL_S = 10;
constexpr int DEFAULT_VHS_TRACKING_LINE_MIN_COUNT = 2;
constexpr int DEFAULT_VHS_TRACKING_LINE_MAX_COUNT = 3;
constexpr int VHS_TRACKING_LINE_ARRAY_CAP = 8;
constexpr int DEFAULT_VHS_TRACKING_LINE_GAP_PX = 3;
constexpr int DEFAULT_VHS_TRACKING_MIN_THICKNESS_PX = 2;
constexpr int DEFAULT_VHS_TRACKING_MAX_THICKNESS_PX = 4;
constexpr double DEFAULT_VHS_TRACKING_JITTER_MIN_PX = 1.0;
constexpr double DEFAULT_VHS_TRACKING_JITTER_MAX_PX = 4.5;
constexpr double DEFAULT_VHS_TRACKING_BRIGHTEN = 0.00; // 0..1
constexpr double VHS_CHROMA_MAX_SHIFT_PX = 40.0;       // full-scale (100) horizontal split for each channel
constexpr int DEFAULT_VHS_SMEAR_AMOUNT = 10;           // 0..100
constexpr double VHS_SMEAR_MAX_TAPS = 20.0;            // longest streak reach at 100 (doubled)
constexpr double VHS_SMEAR_TAP_STEP_PX = 1.15;         // px between each streak tap
constexpr double VHS_SMEAR_TAP_DECAY = 0.78;           // per-tap weight falloff
constexpr double VHS_SMEAR_MAX_RIPPLE_PX = 6.0;        // max wobble amplitude at 100 (doubled)
constexpr double VHS_SMEAR_BURST_SPLIT_PX = 12.0;      // max standalone red/blue split during a burst at 100
constexpr double VHS_SMEAR_NOISE_BAND_PX = 16.0;       // coarse noise wavelength, px
constexpr double VHS_SMEAR_NOISE_BAND2_PX = 6.0;       // fine noise wavelength, px (adds texture/chaos)
constexpr double VHS_SMEAR_NOISE_BAND3_PX = 2.5;       // extra-fine, fast-moving wavelength, px (jittery chaos)
constexpr double VHS_SMEAR_PHASE_SPEED = 0.35;         // noise "time" advance per animation tick (faster = quicker ripple)
constexpr double VHS_SMEAR_BAND_MIN_FRAC = 0.10;       // burst band height, min fraction of card height
constexpr double VHS_SMEAR_BAND_MAX_FRAC = 0.25;       // burst band height, max fraction of card height
constexpr double VHS_SMEAR_BAND_MIN_PX = 20.0;         // burst band height floor, so small cards still show it
constexpr double DEFAULT_VHS_SMEAR_BURST_MIN_S = 3.0;  // shortest plateau (full-strength) duration
constexpr double DEFAULT_VHS_SMEAR_BURST_MAX_S = 6.0;  // longest plateau (full-strength) duration
constexpr double VHS_SMEAR_FADE_IN_S = 0.5;            // ramp in/out duration, on top of the plateau duration
constexpr double VHS_SMEAR_FADE_OUT_S = 0.3;
constexpr double DEFAULT_VHS_SMEAR_MIN_INTERVAL_S = 2.0; // shortest gap between bursts
constexpr double DEFAULT_VHS_SMEAR_MAX_INTERVAL_S = 6.0; // longest gap between bursts
constexpr double VHS_SMEAR_EDGE_FEATHER_PX = 8.0;        // soft fade-in/out width at the band's top/bottom edges
constexpr int DEFAULT_VHS_GLITCH_CHANCE_PCT = 8;
constexpr int DEFAULT_VHS_GLITCH_MAX_BANDS = 4;
constexpr int DEFAULT_VHS_GRAIN_AMOUNT = 50;

constexpr int DEFAULT_EIGHTMM_INTENSITY = 50;
constexpr double DEFAULT_EIGHTMM_VIGNETTE_STRENGTH = 0.55;
constexpr double DEFAULT_EIGHTMM_WARMTH = 18.0; // max 255
constexpr double DEFAULT_EIGHTMM_LIGHT_LEAK_ALPHA = 0.50;
constexpr const char *DEFAULT_EIGHTMM_LIGHT_LEAK_POSITION = "none";
constexpr int DEFAULT_EIGHTMM_LIGHT_LEAK_INTENSITY = 50; // 1..100
constexpr double DEFAULT_EIGHTMM_WEAVE_PX = 0.2;
constexpr double DEFAULT_EIGHTMM_FLICKER = 0.18;      // 0..1
constexpr int DEFAULT_EIGHTMM_SCRATCH_INTENSITY = 45; // 0..100
constexpr int DEFAULT_EIGHTMM_DUST_INTENSITY = 45;    // 0..100
constexpr int DEFAULT_EIGHTMM_SCRATCH_MAX_COUNT = 14;
constexpr int DEFAULT_EIGHTMM_DUST_MAX_COUNT = 150;

constexpr int DEFAULT_DUOTONE_INTENSITY = 100;
constexpr int DEFAULT_DUOTONE_SHADOW_COLOR = 0xFF1B1035;
constexpr int DEFAULT_DUOTONE_HIGHLIGHT_COLOR = 0xFFFF5FA2;

constexpr int DEFAULT_BW_DESATURATION = 100;
constexpr int DEFAULT_BW_CONTRAST = 20;
constexpr double DEFAULT_BW_VIGNETTE_STRENGTH = 0.0;

constexpr int DEFAULT_GLITCH_INTENSITY = 50;
constexpr int DEFAULT_GLITCH_PIXEL_SORT_CHANCE = 15; // 0..100
constexpr int DEFAULT_GLITCH_PIXEL_SORT_MAX_ROWS = 6;
constexpr int DEFAULT_GLITCH_PIXEL_SORT_THRESHOLD = 35; // 0..100
constexpr int DEFAULT_GLITCH_TEAR_CHANCE = 20;          // 0..100
constexpr int DEFAULT_GLITCH_TEAR_MAX_COUNT = 3;
constexpr int DEFAULT_GLITCH_TEAR_MAX_HEIGHT = 14;       // px
constexpr int DEFAULT_GLITCH_TEAR_MAX_OFFSET = 30;       // px
constexpr int DEFAULT_GLITCH_TEAR_DUPLICATE_CHANCE = 35; // 0..100
constexpr int DEFAULT_GLITCH_CHANNEL_BLOCK_CHANCE = 20;  // 0..100
constexpr int DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_COUNT = 4;
constexpr int DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_SIZE = 40;   // px
constexpr int DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_OFFSET = 12; // px

constexpr int VU_MAX_BAR_COUNT = 50;
constexpr int VU_BAR_GAP = 3;
constexpr int VU_GAP_BEFORE_TEXT = 10;

constexpr int DEFAULT_PROGRESS_BAR_HEIGHT = 6;
constexpr int DEFAULT_PROGRESS_BAR_GAP = 6; // gap between artist text and the bar
constexpr int PROGRESS_UPDATE_MS = 1000;    // how often the bar redraws

constexpr int TRACK_CHANGE_TRANSITION_MS = 300;

constexpr int AUTOHIDE_FADE_MS = 1000;
constexpr int DEFAULT_AUTOHIDE_AFTER_S = 5;

constexpr int DEFAULT_NOT_PLAYING_AUTOHIDE_AFTER_S = 10;

constexpr bool DEFAULT_ENABLE_BROWSER_MEDIA_SOURCES = false;

//"308046B0AF4A39CB" is firefox, this will hopefully be fixed in the future
//https://bugzilla.mozilla.org/show_bug.cgi?id=2065866
const char *const DEFAULT_MUSIC_SYSTEMS[] = {
	"spotify", "youtube", "ytm", "pear", "applemusic", "cider", "focal", "vlc",
};
const char *const DEFAULT_BROWSER_SOURCES[] = {
	"operagx", "opera", "brave", "safari", "msedge", "explorer", "firefox", "308046B0AF4A39CB", "chrome",
};

ULONG_PTR g_gdiplusToken = 0;

std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (len <= 0)
		return std::wstring();
	std::wstring out(len - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
	return out;
}

// ---------------------------------------------------------------------
// Native SMTC reading (formerly SpotifyBridge.dll + SpotifyReader.dll)
//
// This block replaces the old C++/CLI bridge and its managed SpotifyReader.dll
// assembly. GlobalSystemMediaTransportControlsSessionManager is a WinRT type,
// and WinRT types are directly callable from plain native C++ via C++/WinRT
// (the <winrt/...> headers) -- no .NET runtime, no AssemblyResolve handler,
// and no separate DLL that can fail to load from the wrong directory.
// ---------------------------------------------------------------------

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackInfo;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionTimelineProperties;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::IRandomAccessStreamWithContentType;

struct NativeMediaInfo {
	bool HasTrack;
	int64_t SongDurationTicks;
	int64_t CurrentPlaybackTimeTicks;
	bool IsPlaying;
	char SongName[256];
	char ArtistName[256];
	char AlbumName[256];
	uint8_t *ImageData;
	int ImageLength;
};

std::vector<std::string> LoadStringList(const char *filename)
{
	std::vector<std::string> strings;

	try {
		if (!filename || !*filename)
			return strings;

		char *path = obs_module_file(filename);
		if (!path) {
			blog(LOG_ERROR, "[spotify_now_playing] Failed to resolve path for '%s'", filename);
			return strings;
		}

		std::ifstream file(path);
		bfree(path);

		if (!file.is_open()) {
			blog(LOG_ERROR, "[spotify_now_playing] Failed to open '%s'", filename);
			return strings;
		}

		std::string line;

		while (std::getline(file, line)) {
			// Remove UTF-8 BOM from the first line if present
			if (strings.empty() && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
				line.erase(0, 3);
			}

			// Trim whitespace and commas from both ends
			const size_t start = line.find_first_not_of(" \t\r\n,");
			if (start == std::string::npos)
				continue;

			const size_t end = line.find_last_not_of(" \t\r\n,");

			line = line.substr(start, end - start + 1);

			if (!line.empty())
				strings.push_back(line);
		}
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[spotify_now_playing] Exception loading system sources lists '%s': %s", filename ? filename : "(null)", e.what());

		strings.clear();
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] Unknown exception loading system sources lists '%s'", filename ? filename : "(null)");

		strings.clear();
	}

	return strings;
}

// hstring -> UTF-8, Required to decode unicode text
void CopyHstringToUtf8(const winrt::hstring &src, char *dst, int maxLen)
{
	if (maxLen <= 0)
		return;
	if (src.empty()) {
		dst[0] = '\0';
		return;
	}

	int needed = WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) {
		dst[0] = '\0';
		return;
	}

	std::vector<char> utf8((size_t)needed); // needed includes the terminating null
	WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, utf8.data(), needed, nullptr, nullptr);

	int copyLen = needed - 1; // exclude the null terminator itself from the length check
	if (copyLen > maxLen - 1)
		copyLen = maxLen - 1; // leave room for the null terminator

	if (copyLen > 0)
		memcpy(dst, utf8.data(), (size_t)copyLen);
	dst[copyLen] = '\0';
}

std::wstring ToLowerWide(const std::wstring &s)
{
	std::wstring out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
	return out;
}

void ReadThumbnail(const GlobalSystemMediaTransportControlsSessionMediaProperties &props, NativeMediaInfo *outInfo)
{
	auto thumbRef = props.Thumbnail();
	if (!thumbRef)
		return;

	IRandomAccessStreamWithContentType stream = thumbRef.OpenReadAsync().get();
	if (!stream)
		return;

	uint32_t size = (uint32_t)stream.Size();
	if (size == 0)
		return;

	DataReader reader(stream);
	reader.LoadAsync(size).get();

	std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);
	reader.ReadBytes(winrt::array_view<uint8_t>(buffer.get(), buffer.get() + size));

	outInfo->ImageData = buffer.release();
	outInfo->ImageLength = (int)size;
}

// `manager` may be null if RequestAsync() hasn't succeeded yet -- poll_loop retries creating it every poll until it succeeds.
static bool GetCurrentTrackNative(GlobalSystemMediaTransportControlsSessionManager const &manager, NativeMediaInfo *outInfo, const std::vector<std::wstring> &possibleMusicSystems, const std::vector<std::wstring> &possibleBrowserMediaSources, bool browserSourcesEnabled)
{
	if (outInfo == nullptr) {
		return false;
	}

	outInfo->SongDurationTicks = 0;
	outInfo->CurrentPlaybackTimeTicks = 0;
	outInfo->SongName[0] = '\0';
	outInfo->ArtistName[0] = '\0';
	outInfo->AlbumName[0] = '\0';
	outInfo->IsPlaying = false;
	outInfo->HasTrack = false;
	outInfo->ImageData = nullptr;
	outInfo->ImageLength = 0;

	if (!manager) {
		return false;
	}

	try {
		auto sessions = manager.GetSessions();
		GlobalSystemMediaTransportControlsSession session = nullptr;
		uint32_t sessionCount = sessions.Size();

		struct SessionEntry {
			GlobalSystemMediaTransportControlsSession session;
			std::wstring lowerId;
		};
		std::vector<SessionEntry> entries;
		entries.reserve(sessionCount);
		for (uint32_t i = 0; i < sessionCount; i++) {
			GlobalSystemMediaTransportControlsSession s = sessions.GetAt(i);
			entries.push_back({s, ToLowerWide(std::wstring(s.SourceAppUserModelId().c_str()))});
		}

		auto findMatch = [&](const std::vector<std::wstring> &candidatesWide) -> GlobalSystemMediaTransportControlsSession {
			for (const std::wstring &candidate : candidatesWide) {
				if (candidate.empty())
					continue;
				for (const auto &e : entries) {
					if (e.lowerId.find(candidate) != std::wstring::npos)
						return e.session;
				}
			}
			return nullptr;
		};

		session = findMatch(possibleMusicSystems);

		if (browserSourcesEnabled && !session) {
			session = findMatch(possibleBrowserMediaSources);
		}

		if (!session) {
			return false;
		}

		GlobalSystemMediaTransportControlsSessionMediaProperties props = session.TryGetMediaPropertiesAsync().get();

		if (!props) {
			return false;
		}

		GlobalSystemMediaTransportControlsSessionTimelineProperties timeline = session.GetTimelineProperties();
		GlobalSystemMediaTransportControlsSessionPlaybackInfo playbackInfo = session.GetPlaybackInfo();

		if (!timeline || !playbackInfo) {
			return false;
		}

		outInfo->HasTrack = true;
		outInfo->SongDurationTicks = (timeline.EndTime() - timeline.StartTime()).count();
		outInfo->CurrentPlaybackTimeTicks = timeline.Position().count();
		outInfo->IsPlaying = playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

		CopyHstringToUtf8(props.Title(), outInfo->SongName, 256);
		CopyHstringToUtf8(props.Artist(), outInfo->ArtistName, 256);
		CopyHstringToUtf8(props.AlbumTitle(), outInfo->AlbumName, 256);

		ReadThumbnail(props, outInfo);

		return true;
	} catch (const winrt::hresult_error &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] SMTC read failed: %ls", ex.message().c_str());
		outInfo->HasTrack = false;
		return false;
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] SMTC read failed: %s", ex.what());
		outInfo->HasTrack = false;
		return false;
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] SMTC read failed: unknown exception");
		outInfo->HasTrack = false;
		return false;
	}
}

static bool GetCurrentTrackSafe(GlobalSystemMediaTransportControlsSessionManager const &manager, NativeMediaInfo *outInfo, const std::vector<std::wstring> &possibleMusicSystems, const std::vector<std::wstring> &possibleBrowserMediaSources, bool browserSourcesEnabled)
{
	__try {
		return GetCurrentTrackNative(manager, outInfo, possibleMusicSystems, possibleBrowserMediaSources, browserSourcesEnabled);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) {
			_resetstkoflw();
		}
		if (outInfo) {
			outInfo->HasTrack = false;
		}
		blog(LOG_ERROR, "[spotify_now_playing] SMTC read failed: structured exception 0x%08lX", GetExceptionCode());
		return false;
	}
}

void FreeImageBuffer(uint8_t *buffer)
{
	delete[] buffer;
}

// Ensures obs_leave_graphics() is always paired with obs_enter_graphics()
struct ScopedGraphics {
	ScopedGraphics() { obs_enter_graphics(); }
	~ScopedGraphics() { obs_leave_graphics(); }
	ScopedGraphics(const ScopedGraphics &) = delete;
	ScopedGraphics &operator=(const ScopedGraphics &) = delete;
};

void AddRoundedRect(GraphicsPath &path, const Rect &r, int radius)
{
	int d = radius * 2;
	path.Reset();
	path.AddArc(r.X, r.Y, d, d, 180, 90);
	path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
	path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
	path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
	path.CloseFigure();
}

Color ObsColorToGdip(long long packed)
{
	uint32_t v = (uint32_t)packed;
	BYTE r = (BYTE)(v & 0xFF);
	BYTE g = (BYTE)((v >> 8) & 0xFF);
	BYTE b = (BYTE)((v >> 16) & 0xFF);
	BYTE a = (BYTE)((v >> 24) & 0xFF);
	return Color(a, r, g, b);
}

Color ObsColorToGdipWithAlpha(long long packed, int opacityPercent)
{
	uint32_t v = (uint32_t)packed;
	BYTE r = (BYTE)(v & 0xFF);
	BYTE g = (BYTE)((v >> 8) & 0xFF);
	BYTE b = (BYTE)((v >> 16) & 0xFF);
	int clampedPct = std::clamp(opacityPercent, 0, 100);
	BYTE a = (BYTE)((clampedPct * 255 + 50) / 100); // round to nearest
	return Color(a, r, g, b);
}

FontStyle ParseFontStyle(const std::string &style, int flags)
{
	// Prefer explicit OBS font flags when present.
	const int OBS_FONT_BOLD_FLAG = 1 << 0;
	const int OBS_FONT_ITALIC_FLAG = 1 << 1;
	if (flags & OBS_FONT_BOLD_FLAG && flags & OBS_FONT_ITALIC_FLAG)
		return FontStyleBoldItalic;
	if (flags & OBS_FONT_BOLD_FLAG)
		return FontStyleBold;
	if (flags & OBS_FONT_ITALIC_FLAG)
		return FontStyleItalic;

	bool bold = style.find("Bold") != std::string::npos;
	bool italic = style.find("Italic") != std::string::npos;
	if (bold && italic)
		return FontStyleBoldItalic;
	if (bold)
		return FontStyleBold;
	if (italic)
		return FontStyleItalic;
	return FontStyleRegular;
}

void PaintTextRun(Graphics &g, const std::wstring &text, Font &font, Brush &fillBrush, const RectF &layoutRect, StringFormat &sf, bool outlineEnabled, float outlineWidthPx, const Color &outlineColor)
{
	if (!outlineEnabled || outlineWidthPx <= 0.0f) {
		g.DrawString(text.c_str(), -1, &font, layoutRect, &sf, &fillBrush);
		return;
	}

	FontFamily fam;
	font.GetFamily(&fam);

	GraphicsPath path;
	path.SetFillMode(FillModeWinding);
	path.AddString(text.c_str(), -1, &fam, font.GetStyle(), font.GetSize(), layoutRect, &sf);

	Pen outlinePen(outlineColor, outlineWidthPx * 2.0f);
	outlinePen.SetLineJoin(LineJoinRound);
	outlinePen.SetStartCap(LineCapRound);
	outlinePen.SetEndCap(LineCapRound);

	SmoothingMode prevSmoothing = g.GetSmoothingMode();
	g.SetSmoothingMode(SmoothingModeAntiAlias);

	g.DrawPath(&outlinePen, &path);
	g.FillPath(&fillBrush, &path);

	g.SetSmoothingMode(prevSmoothing);
}

struct ScrollMeasureCache {
	std::wstring text;
	Font *font = nullptr;
	RectF measured{};
};

void DrawScrollableLine(Graphics &g, const std::wstring &text, Font &font, Brush &brush, const RectF &bounds, double scrollOffsetPx, bool centerWhenStatic, bool outlineEnabled, float outlineWidthPx, const Color &outlineColor, bool *outNeedsScroll, double *outAvgCharPx, double *outMaxOffsetPx, ScrollMeasureCache *measureCache = nullptr)
{
	*outNeedsScroll = false;
	*outMaxOffsetPx = 0.0;
	if (text.empty())
		return;

	std::unique_ptr<StringFormat> sfClone(StringFormat::GenericTypographic()->Clone());
	StringFormat defaultFallback;
	StringFormat &sf = sfClone ? *sfClone : defaultFallback;
	sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);

	RectF measured;
	if (measureCache && measureCache->font == &font && measureCache->text == text) {
		measured = measureCache->measured;
	} else {
		g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &sf, &measured);
		if (measureCache) {
			measureCache->text = text;
			measureCache->font = &font;
			measureCache->measured = measured;
		}
	}
	*outAvgCharPx = std::max(1.0, (double)measured.Width / (double)text.length());

	if (measured.Width <= bounds.Width) {
		if (centerWhenStatic)
			sf.SetAlignment(StringAlignmentCenter);
		sf.SetTrimming(StringTrimmingEllipsisCharacter); // safety net
		PaintTextRun(g, text, font, brush, bounds, sf, outlineEnabled, outlineWidthPx, outlineColor);
		return;
	}

	*outNeedsScroll = true;
	*outMaxOffsetPx = (double)(measured.Width - bounds.Width);

	double offset = std::clamp(scrollOffsetPx, 0.0, *outMaxOffsetPx);

	Region savedClip;
	g.GetClip(&savedClip);

	float outlinePad = (outlineEnabled && outlineWidthPx > 0.0f) ? outlineWidthPx : 0.0f;
	RectF clipRect = bounds;
	clipRect.X -= outlinePad;
	clipRect.Width += outlinePad * 2.0f;
	g.SetClip(clipRect);

	RectF r = bounds;
	r.X -= (REAL)offset;
	r.Width = measured.Width + 4.0f; // wide enough for the full text

	PaintTextRun(g, text, font, brush, r, sf, outlineEnabled, outlineWidthPx, outlineColor);

	g.SetClip(&savedClip);
}

struct CachedFont {
	std::unique_ptr<Font> font;
	std::string face;
	std::string style;
	int size = -1;
	int flags = -1;
};

Font *EnsureFont(CachedFont &cache, const std::string &face, const std::string &style, int size, int flags)
{
	if (!cache.font || cache.face != face || cache.style != style || cache.size != size || cache.flags != flags) {
		FontFamily requestedFam(Utf8ToWide(face).c_str());
		const FontFamily *fam = &requestedFam;
		if (requestedFam.GetLastStatus() != Ok)
			fam = FontFamily::GenericSansSerif();

		FontStyle gdiStyle = ParseFontStyle(style, flags);
		cache.font = std::make_unique<Font>(fam, (REAL)size, gdiStyle, UnitPixel);

		cache.face = face;
		cache.style = style;
		cache.size = size;
		cache.flags = flags;
	}
	return cache.font.get();
}

void BlendPixelBuffers(const std::vector<uint8_t> &from, const std::vector<uint8_t> &to, std::vector<uint8_t> &out, double t)
{
	size_t n = std::min(from.size(), to.size());
	out.resize(n);
	int ti = (int)std::lround(std::clamp(t, 0.0, 1.0) * 255.0);
	for (size_t i = 0; i < n; i++) {
		int a = from[i];
		int b = to[i];
		out[i] = (uint8_t)(a + ((b - a) * ti) / 255);
	}
}

void ScaleAlphaChannel(std::vector<uint8_t> &pixels, float alpha)
{
	if (alpha >= 0.999f)
		return;
	int mul = std::clamp((int)std::lround(alpha * 255.0f), 0, 255);
	for (size_t i = 3; i < pixels.size(); i += 4)
		pixels[i] = (uint8_t)(((int)pixels[i] * mul) / 255);
}

struct GlitchSortRow {
	int y = 0;
	float center = 128.0f; // 0..255
};

struct GlitchTear {
	int y = 0;
	int h = 0;
	int offsetX = 0;
	int srcY = 0;
};

struct GlitchChannelBlock {
	int x = 0, y = 0, w = 0, h = 0;
	int channel = 0;
	int offsetX = 0, offsetY = 0;
};

} // namespace

// ---------------------------------------------------------------------
// AppearanceSettings macro
// ---------------------------------------------------------------------
#define APPEARANCE_SETTINGS_FIELDS(X) \
	X(long long, title_color, DEFAULT_COLOR_WHITE) \
	X(long long, artist_color, DEFAULT_COLOR_WHITE) \
	X(long long, bg_color, 0) \
	X(int, bg_opacity, DEFAULT_BG_OPACITY) \
	X(bool, use_bg_image, false) \
	X(std::string, bg_image_path, "") \
	X(bool, use_album_art_as_bg, false) \
	X(int, album_art_bg_blur_pct, DEFAULT_ALBUM_ART_BG_BLUR_PCT) \
	X(int, background_corner_radius, DEFAULT_BACKGROUND_CORNER_RADIUS) \
	X(int, album_art_corner_radius, DEFAULT_ALBUM_ART_CORNER_RADIUS) \
	X(std::string, title_font_face, DEFAULT_TITLE_FONT_FACE) \
	X(std::string, title_font_style, DEFAULT_TITLE_FONT_STYLE) \
	X(int, title_font_size, DEFAULT_TITLE_FONT_SIZE) \
	X(int, title_font_flags, DEFAULT_TITLE_FONT_FLAGS) \
	X(std::string, artist_font_face, DEFAULT_ARTIST_FONT_FACE) \
	X(std::string, artist_font_style, DEFAULT_ARTIST_FONT_STYLE) \
	X(int, artist_font_size, DEFAULT_ARTIST_FONT_SIZE) \
	X(int, artist_font_flags, DEFAULT_ARTIST_FONT_FLAGS) \
	X(int, card_w, DEFAULT_CARD_W) \
	X(int, card_h, DEFAULT_CARD_H) \
	X(int, text_offset_y, 0) \
	X(int, progress_bar_gap, DEFAULT_PROGRESS_BAR_GAP) \
	X(int, progress_bar_height, DEFAULT_PROGRESS_BAR_HEIGHT) \
	X(int, scroll_speed_ms, DEFAULT_SCROLL_SPEED_MS) \
	X(bool, browser_media_source_enabled, DEFAULT_ENABLE_BROWSER_MEDIA_SOURCES) \
	X(bool, vu_meter_enabled, true) \
	X(long long, vu_color, DEFAULT_VU_COLOR) \
	X(int, vu_update_ms, DEFAULT_VU_UPDATE_SPEED_MS) \
	X(int, vu_randomness, DEFAULT_VU_RANDOMNESS) \
	X(int, vu_width, DEFAULT_VU_WIDTH) \
	X(int, vu_height, DEFAULT_VU_HEIGHT) \
	X(int, vu_bar_count, DEFAULT_VU_BAR_COUNT) \
	X(bool, vu_horizontal, false) \
	X(bool, vertical_layout, false) \
	X(bool, show_album_name, false) \
	X(bool, show_goat_placeholder, true) \
	X(bool, show_plugin_attribution, true) \
	X(bool, hide_album_art, false) \
	X(bool, show_progress_bar, true) \
	X(long long, progress_fill_color, DEFAULT_COLOR_WHITE) \
	X(long long, progress_bg_color, DEFAULT_COLOR_DARK_GREY) \
	X(bool, track_change_animation_enabled, true) \
	X(bool, autohide_enabled, false) \
	X(int, autohide_after_s, DEFAULT_AUTOHIDE_AFTER_S) \
	X(bool, autohide_when_not_playing, false) \
	X(bool, title_outline_enabled, false) \
	X(int, title_outline_size, DEFAULT_TEXT_OUTLINE_SIZE_PX) \
	X(long long, title_outline_color, DEFAULT_COLOR_BLACK) \
	X(bool, artist_outline_enabled, false) \
	X(int, artist_outline_size, DEFAULT_TEXT_OUTLINE_SIZE_PX) \
	X(long long, artist_outline_color, DEFAULT_COLOR_BLACK) \
	X(std::string, card_style, "none") \
	X(int, vhs_intensity, DEFAULT_VHS_INTENSITY) \
	X(int, vhs_chroma_aberration, DEFAULT_VHS_CHROMA_ABERRATION) \
	X(int, vhs_smear_amount, DEFAULT_VHS_SMEAR_AMOUNT) \
	X(double, vhs_smear_burst_min_s, DEFAULT_VHS_SMEAR_BURST_MIN_S) \
	X(double, vhs_smear_burst_max_s, DEFAULT_VHS_SMEAR_BURST_MAX_S) \
	X(double, vhs_smear_min_interval_s, DEFAULT_VHS_SMEAR_MIN_INTERVAL_S) \
	X(double, vhs_smear_max_interval_s, DEFAULT_VHS_SMEAR_MAX_INTERVAL_S) \
	X(int, vhs_scanline_spacing, DEFAULT_VHS_SCANLINE_SPACING_PX) \
	X(int, vhs_scanline_intensity, DEFAULT_VHS_SCANLINE_INTENSITY) \
	X(int, vhs_tracking_min_interval_s, DEFAULT_VHS_TRACKING_MIN_INTERVAL_S) \
	X(int, vhs_tracking_max_interval_s, DEFAULT_VHS_TRACKING_MAX_INTERVAL_S) \
	X(int, vhs_tracking_line_min_count, DEFAULT_VHS_TRACKING_LINE_MIN_COUNT) \
	X(int, vhs_tracking_line_max_count, DEFAULT_VHS_TRACKING_LINE_MAX_COUNT) \
	X(int, vhs_tracking_line_gap, DEFAULT_VHS_TRACKING_LINE_GAP_PX) \
	X(int, vhs_tracking_min_thickness, DEFAULT_VHS_TRACKING_MIN_THICKNESS_PX) \
	X(int, vhs_tracking_max_thickness, DEFAULT_VHS_TRACKING_MAX_THICKNESS_PX) \
	X(double, vhs_tracking_jitter_min, DEFAULT_VHS_TRACKING_JITTER_MIN_PX) \
	X(double, vhs_tracking_jitter_max, DEFAULT_VHS_TRACKING_JITTER_MAX_PX) \
	X(double, vhs_tracking_brighten, DEFAULT_VHS_TRACKING_BRIGHTEN) \
	X(int, vhs_glitch_chance_pct, DEFAULT_VHS_GLITCH_CHANCE_PCT) \
	X(int, vhs_glitch_max_bands, DEFAULT_VHS_GLITCH_MAX_BANDS) \
	X(int, vhs_grain_amount, DEFAULT_VHS_GRAIN_AMOUNT) \
	X(int, eightmm_intensity, DEFAULT_EIGHTMM_INTENSITY) \
	X(double, eightmm_vignette_strength, DEFAULT_EIGHTMM_VIGNETTE_STRENGTH) \
	X(double, eightmm_warmth, DEFAULT_EIGHTMM_WARMTH) \
	X(double, eightmm_light_leak_alpha, DEFAULT_EIGHTMM_LIGHT_LEAK_ALPHA) \
	X(std::string, eightmm_light_leak_position, DEFAULT_EIGHTMM_LIGHT_LEAK_POSITION) \
	X(int, eightmm_light_leak_intensity, DEFAULT_EIGHTMM_LIGHT_LEAK_INTENSITY) \
	X(double, eightmm_weave_px, DEFAULT_EIGHTMM_WEAVE_PX) \
	X(double, eightmm_flicker, DEFAULT_EIGHTMM_FLICKER) \
	X(int, eightmm_scratch_intensity, DEFAULT_EIGHTMM_SCRATCH_INTENSITY) \
	X(int, eightmm_dust_intensity, DEFAULT_EIGHTMM_DUST_INTENSITY) \
	X(int, eightmm_scratch_max_count, DEFAULT_EIGHTMM_SCRATCH_MAX_COUNT) \
	X(int, eightmm_dust_max_count, DEFAULT_EIGHTMM_DUST_MAX_COUNT) \
	X(long long, duotone_shadow_color, DEFAULT_DUOTONE_SHADOW_COLOR) \
	X(long long, duotone_highlight_color, DEFAULT_DUOTONE_HIGHLIGHT_COLOR) \
	X(int, duotone_intensity, DEFAULT_DUOTONE_INTENSITY) \
	X(int, bw_desaturation, DEFAULT_BW_DESATURATION) \
	X(int, bw_contrast, DEFAULT_BW_CONTRAST) \
	X(double, bw_vignette_strength, DEFAULT_BW_VIGNETTE_STRENGTH) \
	X(int, glitch_intensity, DEFAULT_GLITCH_INTENSITY) \
	X(int, glitch_pixel_sort_chance, DEFAULT_GLITCH_PIXEL_SORT_CHANCE) \
	X(int, glitch_pixel_sort_max_rows, DEFAULT_GLITCH_PIXEL_SORT_MAX_ROWS) \
	X(int, glitch_pixel_sort_threshold, DEFAULT_GLITCH_PIXEL_SORT_THRESHOLD) \
	X(int, glitch_tear_chance, DEFAULT_GLITCH_TEAR_CHANCE) \
	X(int, glitch_tear_max_count, DEFAULT_GLITCH_TEAR_MAX_COUNT) \
	X(int, glitch_tear_max_height, DEFAULT_GLITCH_TEAR_MAX_HEIGHT) \
	X(int, glitch_tear_max_offset, DEFAULT_GLITCH_TEAR_MAX_OFFSET) \
	X(int, glitch_tear_duplicate_chance, DEFAULT_GLITCH_TEAR_DUPLICATE_CHANCE) \
	X(int, glitch_channel_block_chance, DEFAULT_GLITCH_CHANNEL_BLOCK_CHANCE) \
	X(int, glitch_channel_block_max_count, DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_COUNT) \
	X(int, glitch_channel_block_max_size, DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_SIZE) \
	X(int, glitch_channel_block_max_offset, DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_OFFSET)

struct spotify_source {
	obs_source_t *source = nullptr;

	std::thread poll_thread;
	std::atomic<bool> running{false};
	std::atomic<bool> is_active{false}; // true only while this source's scene is part of the live/program output

	std::vector<std::string> musicSystemStrings;
	std::vector<std::string> browserMediaSourceStrings;
	std::vector<std::wstring> kPossibleMusicSystems;
	std::vector<std::wstring> kPossibleBrowserMediaSources;

	std::mutex settings_mutex;
#define X(type, name, def) type name = def;
	APPEARANCE_SETTINGS_FIELDS(X)
#undef X
	std::atomic<bool> settings_dirty{true};

	std::mutex bitmap_mutex;
	std::vector<uint8_t> pending_pixels;
	uint32_t pending_w = 0, pending_h = 0;
	std::atomic<bool> new_bitmap_ready{false};

	gs_texture_t *texture = nullptr;
	uint32_t tex_w = 0, tex_h = 0;

	std::string last_song;
	std::string last_artist;
	std::unique_ptr<Image> cached_art_image;
	std::vector<uint8_t> last_art_bytes;
	bool have_track = false;

	std::unique_ptr<Bitmap> cached_blurred_art;
	bool cached_blurred_art_valid = false;

	bool title_needs_scroll = false;
	bool artist_needs_scroll = false;
	double title_scroll_px = 0.0;
	double artist_scroll_px = 0.0;
	double title_avg_char_px = 8.0;
	double artist_avg_char_px = 7.0;
	double title_scroll_max_px = 0.0;
	double artist_scroll_max_px = 0.0;
	bool title_scroll_paused_at_end = false;

	std::string cached_wtitle_src;
	std::wstring cached_wtitle;
	std::string cached_wartist_src;
	std::wstring cached_wartist;

	ScrollMeasureCache title_measure_cache;
	ScrollMeasureCache artist_measure_cache;
	bool artist_scroll_paused_at_end = false;
	bool title_scroll_paused_at_start = false;
	bool artist_scroll_paused_at_start = false;
	std::chrono::steady_clock::time_point title_pause_start{};
	std::chrono::steady_clock::time_point artist_pause_start{};
	std::chrono::steady_clock::time_point last_scroll_tick{};

	double vu_bar_frac[VU_MAX_BAR_COUNT] = {0.0}; // 0..1, scaled to pixel height/length at draw time
	bool is_playing = false;
	bool vu_was_playing = false;
	std::chrono::steady_clock::time_point last_vu_tick{};
	std::mt19937 vu_rng{std::random_device{}()};

	std::chrono::steady_clock::time_point last_vhs_tick{};
	std::mt19937 vhs_rng{std::random_device{}()};
	int vhs_scanline_offset = 0;
	float vhs_smear_phase = 0.0f;
	bool vhs_smear_active = false;
	int vhs_smear_band_y = 0;
	int vhs_smear_band_h = 0;
	std::chrono::steady_clock::time_point vhs_smear_burst_start{};
	std::chrono::steady_clock::time_point vhs_smear_plateau_end{};
	std::chrono::steady_clock::time_point vhs_smear_burst_end{};
	std::chrono::steady_clock::time_point vhs_smear_next_start{};
	int vhs_noise_offset_x = 0;
	int vhs_noise_offset_y = 0;
	bool vhs_glitch_active = false;
	int vhs_glitch_y = 0;
	std::unique_ptr<Bitmap> vhs_noise_texture;
	bool vhs_tracking_active = false;
	int vhs_tracking_line_y = 0;
	int vhs_tracking_line_count = 3;
	float vhs_tracking_shift_dir[VHS_TRACKING_LINE_ARRAY_CAP] = {0.0f};
	std::chrono::steady_clock::time_point vhs_tracking_next_start{};

	std::chrono::steady_clock::time_point last_eightmm_tick{};
	std::mt19937 eightmm_rng{std::random_device{}()};
	int eightmm_noise_offset_x = 0;
	int eightmm_noise_offset_y = 0;
	float eightmm_weave_offset = 0.0f;
	float eightmm_flicker_offset = 0.0f;
	std::unique_ptr<Bitmap> eightmm_noise_texture;

	std::chrono::steady_clock::time_point last_glitch_tick{};
	std::mt19937 glitch_rng{std::random_device{}()};
	std::vector<GlitchSortRow> glitch_sort_rows;
	std::vector<GlitchTear> glitch_tears;
	std::vector<GlitchChannelBlock> glitch_channel_blocks;

	int64_t song_duration_ticks = 0; // .NET TimeSpan ticks (100ns each)
	int64_t playback_position_ticks = 0;
	std::chrono::steady_clock::time_point position_sample_time{};
	std::chrono::steady_clock::time_point last_progress_tick{};
	int64_t max_displayed_position_ticks = 0;

	std::unique_ptr<Image> goat_image;
	bool goat_image_load_attempted = false;

	std::unique_ptr<Image> cached_bg_image;
	std::string cached_bg_image_path;

	std::unique_ptr<Bitmap> cached_bitmap;
	int cached_bitmap_w = 0;
	int cached_bitmap_h = 0;

	std::vector<uint8_t> overlay_scratch_pixels;
	std::vector<uint8_t> vhs_ca_red_blur_scratch;
	std::vector<uint8_t> vhs_ca_blue_blur_scratch;

	std::vector<std::array<uint8_t, 4>> glitch_sort_scratch;

	CachedFont title_font_cache;
	CachedFont artist_font_cache;

	bool transition_active = false;
	std::vector<uint8_t> transition_from_pixels;
	std::vector<uint8_t> transition_to_pixels;
	uint32_t transition_w = 0, transition_h = 0;
	std::chrono::steady_clock::time_point transition_start{};

	float autohide_alpha = 1.0f;
	std::chrono::steady_clock::time_point autohide_reference_time{};
	std::chrono::steady_clock::time_point last_autohide_tick{};

	std::chrono::steady_clock::time_point last_playing_time{};
};

struct AppearanceSettings {
#define X(type, name, def) type name = def;
	APPEARANCE_SETTINGS_FIELDS(X)
#undef X
};

static void DrawVuMeter(Graphics &g, spotify_source *ctx, const AppearanceSettings &s, const Rect &blockRect)
{
	if (!s.vu_meter_enabled || blockRect.Width <= 0 || blockRect.Height <= 0)
		return;

	int barCount = std::clamp(s.vu_bar_count, 1, VU_MAX_BAR_COUNT);
	Color vuColor = ObsColorToGdip(s.vu_color);
	SolidBrush vuBrush(vuColor);

	int totalGap = (barCount - 1) * VU_BAR_GAP;

	if (!s.vu_horizontal) {
		int barThickness = std::max(1, (blockRect.Width - totalGap) / barCount);
		int baselineY = blockRect.Y + blockRect.Height;

		for (int i = 0; i < barCount; i++) {
			double frac = std::clamp(ctx->vu_bar_frac[i], 0.0, 1.0);
			int barH = (int)std::lround(2.0 + frac * (double)(blockRect.Height - 2));
			if (barH < 2)
				barH = 2;
			int barX = blockRect.X + i * (barThickness + VU_BAR_GAP);
			int barY = baselineY - barH;

			Rect barRect(barX, barY, barThickness, barH);
			GraphicsPath barPath;
			AddRoundedRect(barPath, barRect, std::min(2, barThickness / 2));
			g.FillPath(&vuBrush, &barPath);
		}
	} else {
		int barThickness = std::max(1, (blockRect.Height - totalGap) / barCount);

		for (int i = 0; i < barCount; i++) {
			double frac = std::clamp(ctx->vu_bar_frac[i], 0.0, 1.0);
			int barLen = (int)std::lround(2.0 + frac * (double)(blockRect.Width - 2));
			if (barLen < 2)
				barLen = 2;
			int barY = blockRect.Y + i * (barThickness + VU_BAR_GAP);
			int barX = blockRect.X;

			Rect barRect(barX, barY, barLen, barThickness);
			GraphicsPath barPath;
			AddRoundedRect(barPath, barRect, std::min(2, barThickness / 2));
			g.FillPath(&vuBrush, &barPath);
		}
	}
}

static void DrawProgressBar(Graphics &g, spotify_source *ctx, const AppearanceSettings &s, const Rect &barRect)
{
	if (!s.show_progress_bar || barRect.Width <= 0 || barRect.Height <= 0)
		return;

	double frac = 0.0;
	if (ctx->song_duration_ticks > 0) {
		int64_t elapsedTicks = ctx->playback_position_ticks;
		if (ctx->is_playing) {
			double elapsedSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - ctx->position_sample_time).count();
			elapsedTicks += (int64_t)(elapsedSeconds * 1.0e7); // 1 tick = 100ns
		}

		// Never let the displayed position move backward
		if (elapsedTicks < ctx->max_displayed_position_ticks)
			elapsedTicks = ctx->max_displayed_position_ticks;
		else
			ctx->max_displayed_position_ticks = elapsedTicks;

		frac = std::clamp((double)elapsedTicks / (double)ctx->song_duration_ticks, 0.0, 1.0);
	}

	Color bgColor = ObsColorToGdip(s.progress_bg_color);
	SolidBrush bgBrush(bgColor);
	GraphicsPath bgPath;
	AddRoundedRect(bgPath, barRect, barRect.Height / 2);
	g.FillPath(&bgBrush, &bgPath);

	int fillWidth = (int)std::lround(barRect.Width * frac);
	if (fillWidth > 0) {
		Rect fillRect(barRect.X, barRect.Y, fillWidth, barRect.Height);
		Color fillColor = ObsColorToGdip(s.progress_fill_color);
		SolidBrush fillBrush(fillColor);
		GraphicsPath fillPath;
		AddRoundedRect(fillPath, fillRect, barRect.Height / 2);
		g.FillPath(&fillBrush, &fillPath);
	}
}

static Image *GetGoatImage(spotify_source *ctx)
{
	if (ctx->goat_image_load_attempted)
		return ctx->goat_image.get();

	ctx->goat_image_load_attempted = true;

	char *path = obs_module_file("goat.png");
	if (!path)
		return nullptr;

	std::wstring wpath = Utf8ToWide(path);
	bfree(path);

	auto img = std::make_unique<Image>(wpath.c_str());
	if (img->GetLastStatus() != Ok)
		return nullptr;

	ctx->goat_image = std::move(img);
	return ctx->goat_image.get();
}

static Image *EnsureBackgroundImage(spotify_source *ctx, const std::string &path)
{
	if (path.empty()) {
		ctx->cached_bg_image.reset();
		ctx->cached_bg_image_path.clear();
		return nullptr;
	}

	if (ctx->cached_bg_image && ctx->cached_bg_image_path == path)
		return ctx->cached_bg_image.get();

	ctx->cached_bg_image.reset();
	ctx->cached_bg_image_path.clear();

	std::wstring wpath = Utf8ToWide(path);

	std::ifstream file(wpath, std::ios::binary | std::ios::ate);
	if (!file)
		return nullptr;

	std::streamsize fileSize = file.tellg();
	if (fileSize <= 0)
		return nullptr;
	file.seekg(0, std::ios::beg);

	std::vector<uint8_t> bytes((size_t)fileSize);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), fileSize))
		return nullptr;
	file.close();

	IStream *stream = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
	if (!stream)
		return nullptr;

	auto img = std::make_unique<Image>(stream);
	stream->Release();

	if (img->GetLastStatus() != Ok)
		return nullptr;

	// Clone so the cached image no longer depends on the (already-released) stream.
	auto cloned = std::unique_ptr<Image>(img->Clone());
	if (!cloned || cloned->GetLastStatus() != Ok)
		return nullptr;

	ctx->cached_bg_image = std::move(cloned);
	ctx->cached_bg_image_path = path;
	return ctx->cached_bg_image.get();
}

static bool ArtBytesDiffer(const std::vector<uint8_t> &cached, const uint8_t *image_data, int image_len)
{
	if (image_data == nullptr || image_len <= 0)
		return !cached.empty();
	if (cached.size() != (size_t)image_len)
		return true;
	return memcmp(cached.data(), image_data, (size_t)image_len) != 0;
}

static void UpdateCachedArt(spotify_source *ctx, const uint8_t *image_data, int image_len)
{
	ctx->cached_blurred_art_valid = false;
	ctx->cached_art_image.reset();
	if (image_data == nullptr || image_len <= 0) {
		ctx->last_art_bytes.clear();
		return;
	}

	IStream *stream = SHCreateMemStream(image_data, (UINT)image_len);
	if (!stream)
		return;

	auto img = std::make_unique<Image>(stream);
	stream->Release();

	if (img->GetLastStatus() != Ok)
		return;

	auto cloned = std::unique_ptr<Image>(img->Clone());
	if (!cloned || cloned->GetLastStatus() != Ok)
		return;

	ctx->cached_art_image = std::move(cloned);
	ctx->last_art_bytes.assign(image_data, image_data + image_len);
}

static void DrawAlbumArtBackground(Graphics &g, spotify_source *ctx, Image *art, GraphicsPath &clipPath, int cardW, int cardH, int blurPct, int opacityPercent)
{
	if (!art)
		return;

	UINT imgW = art->GetWidth();
	UINT imgH = art->GetHeight();
	if (imgW == 0 || imgH == 0 || cardW <= 0 || cardH <= 0)
		return;

	REAL srcW = (REAL)imgW;
	REAL srcCropH = (REAL)cardH * (REAL)imgW / (REAL)cardW;
	if (srcCropH > (REAL)imgH)
		srcCropH = (REAL)imgH; // image isn't tall enough to fully cover; use all of it
	REAL srcY = ((REAL)imgH - srcCropH) / 2.0f;

	ImageAttributes attr;
	if (opacityPercent < 100) {
		REAL a = std::clamp(opacityPercent, 0, 100) / 100.0f;
		Gdiplus::ColorMatrix cm = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, 1};
		attr.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
	}

	Region savedClip;
	g.GetClip(&savedClip);
	g.SetClip(&clipPath);

	RectF destRect(0.0f, 0.0f, (REAL)cardW, (REAL)cardH);
	int pct = std::clamp(blurPct, 0, 100);

	if (ctx->settings_dirty)
		ctx->cached_blurred_art_valid = false;

	bool blurred = false;
	if (pct > 0) {
		if (!ctx->cached_blurred_art || !ctx->cached_blurred_art_valid) {
			auto cropped = std::make_unique<Bitmap>(cardW, cardH, PixelFormat32bppARGB);
			Graphics gc(cropped.get());
			gc.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			gc.SetSmoothingMode(SmoothingModeHighQuality);
			gc.Clear(Color(0, 0, 0, 0));
			gc.DrawImage(art, destRect, 0.0f, srcY, srcW, srcCropH, UnitPixel, nullptr);

			Gdiplus::Blur blurEffect;
			Gdiplus::BlurParams blurParams = {(pct / 100.0f) * 40.0f, FALSE}; // 0-100% -> ~0-40px radius
			if (blurEffect.SetParameters(&blurParams) == Ok && cropped->ApplyEffect(&blurEffect, nullptr) == Ok) {
				ctx->cached_blurred_art = std::move(cropped);
				ctx->cached_blurred_art_valid = true;
			} else {
				ctx->cached_blurred_art.reset();
				ctx->cached_blurred_art_valid = false;
			}
		}

		if (ctx->cached_blurred_art_valid) {
			g.DrawImage(ctx->cached_blurred_art.get(), destRect, 0.0f, 0.0f, (REAL)cardW, (REAL)cardH, UnitPixel, &attr);
			blurred = true;
		}
	}

	if (!blurred)
		g.DrawImage(art, destRect, 0.0f, srcY, srcW, srcCropH, UnitPixel, &attr);

	g.SetClip(&savedClip);
}

static Bitmap *EnsureNoiseTexture(std::unique_ptr<Bitmap> &cache)
{
	if (cache)
		return cache.get();

	auto tex = std::make_unique<Bitmap>(VHS_NOISE_TEXTURE_SIZE, VHS_NOISE_TEXTURE_SIZE, PixelFormat32bppARGB);
	BitmapData bd;
	Rect full(0, 0, VHS_NOISE_TEXTURE_SIZE, VHS_NOISE_TEXTURE_SIZE);
	if (tex->LockBits(&full, ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
		std::mt19937 rng{std::random_device{}()};
		std::uniform_int_distribution<int> shadeDist(0, 255);
		std::uniform_int_distribution<int> alphaDist(0, 90); // keep grain subtle even at full intensity
		for (int y = 0; y < VHS_NOISE_TEXTURE_SIZE; y++) {
			uint8_t *row = (uint8_t *)bd.Scan0 + (size_t)y * bd.Stride;
			for (int x = 0; x < VHS_NOISE_TEXTURE_SIZE; x++) {
				uint8_t shade = (uint8_t)shadeDist(rng);
				uint8_t a = (uint8_t)alphaDist(rng);
				// premultiplied BGRA
				row[x * 4 + 0] = (uint8_t)((shade * a) / 255);
				row[x * 4 + 1] = (uint8_t)((shade * a) / 255);
				row[x * 4 + 2] = (uint8_t)((shade * a) / 255);
				row[x * 4 + 3] = a;
			}
		}
		tex->UnlockBits(&bd);
	}

	cache = std::move(tex);
	return cache.get();
}

static void DrawVhsOverlay(Graphics &g, Bitmap &card, spotify_source *ctx, GraphicsPath &clipPath, int cardW, int cardH, const AppearanceSettings &s)
{
	REAL t = std::clamp(s.vhs_intensity, 0, 100) / 100.0f;
	if (t <= 0.0f)
		return;

	Gdiplus::HueSaturationLightness hsl;
	Gdiplus::HueSaturationLightnessParams hslParams = {0, (INT)(-45.0f * t), 0};
	if (hsl.SetParameters(&hslParams) == Ok)
		card.ApplyEffect(&hsl, nullptr);

	Gdiplus::BrightnessContrast bc;
	Gdiplus::BrightnessContrastParams bcParams = {(INT)(-8.0f * t), (INT)(25.0f * t)};
	if (bc.SetParameters(&bcParams) == Ok)
		card.ApplyEffect(&bc, nullptr);

	Region savedClip;
	g.GetClip(&savedClip);
	g.SetClip(&clipPath);

	REAL ca = std::clamp(s.vhs_chroma_aberration, 0, 100) / 100.0f;
	REAL smearT = std::clamp(s.vhs_smear_amount, 0, 100) / 100.0f;

	double smearEnvelope = 0.0;
	if (smearT > 0.0f && ctx->vhs_smear_active) {
		auto nowDraw = std::chrono::steady_clock::now();
		double fadeInS = std::max(0.05, VHS_SMEAR_FADE_IN_S);
		double fadeOutS = std::max(0.05, VHS_SMEAR_FADE_OUT_S);
		double sinceStart = std::chrono::duration<double>(nowDraw - ctx->vhs_smear_burst_start).count();
		double untilEnd = std::chrono::duration<double>(ctx->vhs_smear_burst_end - nowDraw).count();
		double p;
		if (sinceStart < fadeInS)
			p = std::clamp(sinceStart / fadeInS, 0.0, 1.0);
		else if (untilEnd < fadeOutS)
			p = std::clamp(untilEnd / fadeOutS, 0.0, 1.0);
		else
			p = 1.0;
		smearEnvelope = p * p * (3.0 - 2.0 * p); // smoothstep
	}
	bool smearBurstNow = smearEnvelope > 0.0;
	if ((ca > 0.0f || smearBurstNow) && cardW > 0 && cardH > 0) {
		int rowStart = 0, rowEnd = cardH;
		if (ca <= 0.0f && smearBurstNow) {
			int margin = (int)std::ceil(VHS_SMEAR_EDGE_FEATHER_PX) + 1;
			rowStart = std::clamp(ctx->vhs_smear_band_y - margin, 0, cardH);
			rowEnd = std::clamp(ctx->vhs_smear_band_y + ctx->vhs_smear_band_h + margin, 0, cardH);
		}

		BitmapData bd;
		Rect full(0, 0, cardW, cardH);
		if (rowEnd > rowStart && card.LockBits(&full, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
			ctx->overlay_scratch_pixels.resize((size_t)cardW * cardH * 4);
			std::vector<uint8_t> &src = ctx->overlay_scratch_pixels;
			const uint8_t *srcRow = (const uint8_t *)bd.Scan0;
			for (int y = rowStart; y < rowEnd; y++)
				memcpy(src.data() + (size_t)y * cardW * 4, srcRow + (size_t)y * bd.Stride, (size_t)cardW * 4);
			card.UnlockBits(&bd);

			REAL maxOffsetPx = (REAL)VHS_CHROMA_MAX_SHIFT_PX * ca;

			double rdxGlobal = -(double)maxOffsetPx; // visually shifts red left
			double bdxGlobal = (double)maxOffsetPx;  // visually shifts blue right

			int blurRadius = std::clamp((int)std::lround((double)maxOffsetPx * 0.15), 1, 4);
			ctx->vhs_ca_red_blur_scratch.resize((size_t)cardW * cardH);
			ctx->vhs_ca_blue_blur_scratch.resize((size_t)cardW * cardH);
			std::vector<uint8_t> &redBlur = ctx->vhs_ca_red_blur_scratch;
			std::vector<uint8_t> &blueBlur = ctx->vhs_ca_blue_blur_scratch;
			for (int y = rowStart; y < rowEnd; y++) {
				const uint8_t *rowSrc = &src[(size_t)y * cardW * 4];
				uint8_t *redRow = &redBlur[(size_t)y * cardW];
				uint8_t *blueRow = &blueBlur[(size_t)y * cardW];

				int rSum = 0, bSum = 0, n = 0;
				{
					int hi0 = std::min(cardW - 1, blurRadius);
					for (int xx = 0; xx <= hi0; xx++) {
						const uint8_t *p = rowSrc + (size_t)xx * 4;
						bSum += p[0];
						rSum += p[2];
						n++;
					}
					redRow[0] = (uint8_t)(rSum / n);
					blueRow[0] = (uint8_t)(bSum / n);
				}
				for (int x = 1; x < cardW; x++) {
					int newHi = x + blurRadius;
					int oldLo = x - blurRadius - 1;
					if (newHi <= cardW - 1) {
						const uint8_t *p = rowSrc + (size_t)newHi * 4;
						bSum += p[0];
						rSum += p[2];
						n++;
					}
					if (oldLo >= 0) {
						const uint8_t *p = rowSrc + (size_t)oldLo * 4;
						bSum -= p[0];
						rSum -= p[2];
						n--;
					}
					redRow[x] = (uint8_t)(rSum / n);
					blueRow[x] = (uint8_t)(bSum / n);
				}
			}

			auto samplePlane = [&](const std::vector<uint8_t> &plane, double x, int y) -> uint8_t {
				y = std::clamp(y, 0, cardH - 1);
				int x0 = (int)std::floor(x);
				int x1 = x0 + 1;
				double fx = x - x0;
				double v0 = (x0 >= 0 && x0 < cardW) ? (double)plane[(size_t)y * cardW + (size_t)x0] : 0.0;
				double v1 = (x1 >= 0 && x1 < cardW) ? (double)plane[(size_t)y * cardW + (size_t)x1] : 0.0;
				return (uint8_t)std::lround(v0 + (v1 - v0) * fx);
			};

			int smearTaps = smearT > 0.0f ? std::clamp((int)std::lround((double)smearT * VHS_SMEAR_MAX_TAPS), 1, (int)VHS_SMEAR_MAX_TAPS) : 0;
			double rippleAmp = (double)smearT * VHS_SMEAR_MAX_RIPPLE_PX;
			double burstSplitPx = (double)smearT * VHS_SMEAR_BURST_SPLIT_PX;
			double t = (double)ctx->vhs_smear_phase;

			static const std::array<double, (size_t)VHS_SMEAR_MAX_TAPS + 1> tapWeights = [] {
				std::array<double, (size_t)VHS_SMEAR_MAX_TAPS + 1> w{};
				for (int i = 1; i <= (int)VHS_SMEAR_MAX_TAPS; i++)
					w[i] = std::pow(VHS_SMEAR_TAP_DECAY, (double)i);
				return w;
			}();

			auto latticeHash = [](int i, int salt) -> float {
				uint32_t h = (uint32_t)(i * 374761393 + salt * 668265263);
				h = (h ^ (h >> 13)) * 1274126177u;
				h ^= h >> 16;
				return (float)(h & 0xFFFFFFu) / (float)0x1000000u; // [0,1)
			};
			auto valueNoise1D = [&](float pos, int salt) -> float {
				int i0 = (int)std::floor(pos);
				float f = pos - (float)i0;
				float sm = f * f * (3.0f - 2.0f * f); // smoothstep
				float v0 = latticeHash(i0, salt);
				float v1 = latticeHash(i0 + 1, salt);
				return v0 + (v1 - v0) * sm; // [0,1)
			};
			auto chaosWave = [&](float y, int salt) -> float {
				float n1 = valueNoise1D(y / (float)VHS_SMEAR_NOISE_BAND_PX + (float)t * 0.6f, salt);
				float n2 = valueNoise1D(y / (float)VHS_SMEAR_NOISE_BAND2_PX + (float)t * 1.7f, salt + 97);
				float n3 = valueNoise1D(y / (float)VHS_SMEAR_NOISE_BAND3_PX + (float)t * 3.1f, salt + 251);
				return n1 * 0.5f + n2 * 0.3f + n3 * 0.2f; // [0,1), irregular / non-periodic
			};
			auto smearBandFactor = [&](int y) -> float {
				if (!smearBurstNow)
					return 0.0f;
				float top = (float)ctx->vhs_smear_band_y;
				float bot = (float)(ctx->vhs_smear_band_y + ctx->vhs_smear_band_h);
				float feather = std::max(1.0f, (float)VHS_SMEAR_EDGE_FEATHER_PX);
				float f = std::min((float)y - top, bot - (float)y) / feather;
				f = std::clamp(f, 0.0f, 1.0f);
				return f * f * (3.0f - 2.0f * f);
			};

			auto sampleSmeared = [&](const std::vector<uint8_t> &plane, double baseX, int y, double dirSign, double ripple, float bandF) -> uint8_t {
				if (bandF <= 0.001f)
					return samplePlane(plane, baseX, y);
				uint8_t base = samplePlane(plane, baseX + ripple, y);
				int taps = (int)std::lround(smearTaps * (double)bandF);
				if (taps <= 0)
					return base;
				double sum = (double)base, wsum = 1.0;
				for (int i = 1; i <= taps; i++) {
					double w = tapWeights[i];
					double sampleX = baseX + ripple + dirSign * (double)i * VHS_SMEAR_TAP_STEP_PX;
					sum += (double)samplePlane(plane, sampleX, y) * w;
					wsum += w;
				}
				return (uint8_t)std::lround(sum / wsum);
			};

			if (card.LockBits(&full, ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
				uint8_t *dstBase = (uint8_t *)bd.Scan0;
				for (int y = rowStart; y < rowEnd; y++) {
					float bandF = smearBandFactor(y) * (float)smearEnvelope;  // spatial x temporal fade, combined
					double rdxRow = rdxGlobal - burstSplitPx * (double)bandF; // red trails further left inside the band
					double bdxRow = bdxGlobal + burstSplitPx * (double)bandF; // blue trails further right inside the band

					double rippleBlue = 0.0, rippleRed = 0.0;
					if (bandF > 0.001f) {
						rippleBlue = rippleAmp * ((double)chaosWave((float)y, 11) * 2.0 - 1.0) * (double)bandF;
						rippleRed = rippleAmp * ((double)chaosWave((float)y, 29) * 2.0 - 1.0) * (double)bandF;
					}

					uint8_t *row = dstBase + (size_t)y * bd.Stride;
					for (int x = 0; x < cardW; x++) {
						size_t origIdx = ((size_t)y * cardW + (size_t)x) * 4;
						uint8_t *px = row + (size_t)x * 4;
						px[0] = sampleSmeared(blueBlur, x + bdxRow, y, 1.0, rippleBlue, bandF);
						px[1] = src[origIdx + 1];
						px[2] = sampleSmeared(redBlur, x + rdxRow, y, -1.0, rippleRed, bandF);
						px[3] = src[origIdx + 3];
					}
				}
				card.UnlockBits(&bd);
			}
		}
	}

	if (ctx->vhs_tracking_active && cardH > 4) {
		std::unique_ptr<Bitmap> trackSnap(card.Clone(0, 0, cardW, cardH, PixelFormat32bppARGB));
		if (trackSnap) {
			REAL brighten = (REAL)s.vhs_tracking_brighten;
			Gdiplus::ColorMatrix brightenMatrix = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, brighten, brighten, brighten, 0, 1};
			ImageAttributes brightenAttr;
			brightenAttr.SetColorMatrix(&brightenMatrix, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

			int bandH = s.vhs_tracking_min_thickness + (int)((s.vhs_tracking_max_thickness - s.vhs_tracking_min_thickness) * t);
			int lineCount = std::clamp(ctx->vhs_tracking_line_count, 1, VHS_TRACKING_LINE_ARRAY_CAP);
			REAL jitterRange = (REAL)(s.vhs_tracking_jitter_min + (s.vhs_tracking_jitter_max - s.vhs_tracking_jitter_min) * t);
			for (int i = 0; i < lineCount; i++) {
				int bandY = ctx->vhs_tracking_line_y - i * s.vhs_tracking_line_gap;
				if (bandY < 0 || bandY > cardH - bandH)
					continue;
				REAL shiftPx = ctx->vhs_tracking_shift_dir[i] * jitterRange;
				RectF destBand(shiftPx, (REAL)bandY, (REAL)cardW, (REAL)bandH);
				g.DrawImage(trackSnap.get(), destBand, 0.0f, (REAL)bandY, (REAL)cardW, (REAL)bandH, UnitPixel, &brightenAttr);
			}
		}
	}

	int scanlineSpacing = std::max(1, s.vhs_scanline_spacing);
	// Scaled 3x vs. the original formula: what used to be 100% intensity now sits at
	// ~33%, and 100% now reaches 3x the old maximum scanline darkness (before clamping).
	Color scanColor(std::clamp((int)(70.0f * t * (std::clamp(s.vhs_scanline_intensity, 0, 100) / 100.0f) * 6.0f), 0, 255), 0, 0, 0);
	SolidBrush scanBrush(scanColor);
	for (int y = -scanlineSpacing + ctx->vhs_scanline_offset; y < cardH; y += scanlineSpacing)
		g.FillRectangle(&scanBrush, Rect(0, y, cardW, 1));

	Bitmap *noiseTex = EnsureNoiseTexture(ctx->vhs_noise_texture);
	if (noiseTex) {
		REAL grainAlpha = t * std::clamp(s.vhs_grain_amount, 0, 100) / 100.0f * 2.0f;
		ImageAttributes noiseAttr;
		Gdiplus::ColorMatrix cm = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, grainAlpha, 0, 0, 0, 0, 0, 1};
		noiseAttr.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

		RectF texRect(0.0f, 0.0f, (REAL)VHS_NOISE_TEXTURE_SIZE, (REAL)VHS_NOISE_TEXTURE_SIZE);
		TextureBrush noiseBrush(noiseTex, texRect, &noiseAttr);
		noiseBrush.SetWrapMode(WrapModeTile);
		Matrix m;
		m.Translate((REAL)-ctx->vhs_noise_offset_x, (REAL)-ctx->vhs_noise_offset_y);
		noiseBrush.SetTransform(&m);
		g.FillRectangle(&noiseBrush, Rect(0, 0, cardW, cardH));
	}

	if (ctx->vhs_glitch_active) {
		std::unique_ptr<Bitmap> glitchSnap;
		if (cardH > 4)
			glitchSnap.reset(card.Clone(0, 0, cardW, cardH, PixelFormat32bppARGB));

		REAL jitterRange = (REAL)(s.vhs_tracking_jitter_min + (s.vhs_tracking_jitter_max - s.vhs_tracking_jitter_min) * t);
		std::uniform_int_distribution<int> bandCountDist(1, std::max(1, s.vhs_glitch_max_bands));
		int bandCount = bandCountDist(ctx->vhs_rng);
		for (int b = 0; b < bandCount; b++) {
			std::uniform_int_distribution<int> yJitterDist(-5, 5);
			int baseBandY = std::clamp(ctx->vhs_glitch_y + yJitterDist(ctx->vhs_rng), 0, std::max(0, cardH - 1));

			if (glitchSnap) {
				std::uniform_int_distribution<int> shiftBandHDist(2, 6);
				int shiftBandH = std::min(shiftBandHDist(ctx->vhs_rng), cardH - baseBandY);
				if (shiftBandH > 0) {
					std::uniform_real_distribution<double> dirDist(-1.0, 1.0);
					REAL shiftPx = (REAL)dirDist(ctx->vhs_rng) * jitterRange;
					RectF destBand(shiftPx, (REAL)baseBandY, (REAL)cardW, (REAL)shiftBandH);
					g.DrawImage(glitchSnap.get(), destBand, 0.0f, (REAL)baseBandY, (REAL)cardW, (REAL)shiftBandH, UnitPixel, nullptr);
				}
			}

			std::uniform_int_distribution<int> segWDist(2, std::max(6, cardW / 8));
			std::uniform_int_distribution<int> gapWDist(0, std::max(4, cardW / 10));
			std::uniform_int_distribution<int> segHDist(1, 3);
			std::uniform_int_distribution<int> rowJitterDist(-1, 1);
			std::uniform_int_distribution<int> alphaDist(40, 255);
			std::uniform_int_distribution<int> tintDist(215, 255);
			std::uniform_int_distribution<int> startXDist(-cardW / 10, 0);

			int x = startXDist(ctx->vhs_rng);
			while (x < cardW) {
				int segW = segWDist(ctx->vhs_rng);
				int segH = segHDist(ctx->vhs_rng);
				int segY = std::clamp(baseBandY + rowJitterDist(ctx->vhs_rng), 0, std::max(0, cardH - segH));
				int alpha = std::clamp((int)(alphaDist(ctx->vhs_rng) * t), 0, 255);
				Color segColor(alpha, tintDist(ctx->vhs_rng), tintDist(ctx->vhs_rng), tintDist(ctx->vhs_rng));
				SolidBrush segBrush(segColor);

				int drawX = std::max(0, x);
				int drawW = std::min(segW - (drawX - x), cardW - drawX);
				if (drawW > 0)
					g.FillRectangle(&segBrush, Rect(drawX, segY, drawW, segH));

				x += segW + gapWDist(ctx->vhs_rng);
			}
		}
	}

	g.SetClip(&savedClip);
}

static void DrawEightMmOverlay(Graphics &g, Bitmap &card, spotify_source *ctx, GraphicsPath &clipPath, int cardW, int cardH, const AppearanceSettings &s)
{
	REAL t = std::clamp(s.eightmm_intensity, 0, 100) / 100.0f;
	if (t <= 0.0f || cardW <= 0 || cardH <= 0)
		return;

	Region savedClip;
	g.GetClip(&savedClip);

	if (s.eightmm_weave_px > 0.0 && cardH > 4) {
		std::unique_ptr<Bitmap> weaveSnap(card.Clone(0, 0, cardW, cardH, PixelFormat32bppARGB));
		if (weaveSnap) {
			g.SetClip(&clipPath);
			g.Clear(Color(0, 0, 0, 0));
			RectF weaveDest(0.0f, ctx->eightmm_weave_offset, (REAL)cardW, (REAL)cardH);
			g.DrawImage(weaveSnap.get(), weaveDest, 0.0f, 0.0f, (REAL)cardW, (REAL)cardH, UnitPixel, nullptr);
		}
	}

	REAL warm = ((REAL)s.eightmm_warmth / 255.0f) * t;
	Gdiplus::ColorMatrixEffect warmEffect;
	Gdiplus::ColorMatrix warmMatrix = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, warm, warm * 0.35f, -warm * 0.6f, 0, 1};
	if (warmEffect.SetParameters(&warmMatrix) == Ok)
		card.ApplyEffect(&warmEffect, nullptr);

	Gdiplus::BrightnessContrast bc;
	INT flickerAmt = (INT)(ctx->eightmm_flicker_offset * (REAL)s.eightmm_flicker * 40.0f);
	Gdiplus::BrightnessContrastParams bcParams = {(INT)(8.0f * t) + flickerAmt, (INT)(-15.0f * t)};
	if (bc.SetParameters(&bcParams) == Ok)
		card.ApplyEffect(&bc, nullptr);

	Gdiplus::HueSaturationLightness hsl;
	Gdiplus::HueSaturationLightnessParams hslParams = {0, (INT)(-20.0f * t), 0};
	if (hsl.SetParameters(&hslParams) == Ok)
		card.ApplyEffect(&hsl, nullptr);

	g.SetClip(&clipPath);

	if (s.eightmm_light_leak_position != "none") {
		REAL leakCx = 0.0f, leakCy = 0.0f;
		if (s.eightmm_light_leak_position == "top_left") {
			leakCx = 0.0f;
			leakCy = 0.0f;
		} else if (s.eightmm_light_leak_position == "top_right") {
			leakCx = (REAL)cardW;
			leakCy = 0.0f;
		} else if (s.eightmm_light_leak_position == "bottom_left") {
			leakCx = 0.0f;
			leakCy = (REAL)cardH;
		} else if (s.eightmm_light_leak_position == "bottom_right") {
			leakCx = (REAL)cardW;
			leakCy = (REAL)cardH;
		}

		REAL diag = (REAL)std::sqrt((double)cardW * (double)cardW + (double)cardH * (double)cardH);
		REAL leakT = std::clamp(s.eightmm_light_leak_intensity, 1, 100) / 100.0f;
		REAL leakR = diag * (0.06f + 0.94f * leakT);

		GraphicsPath leakPath;
		leakPath.AddEllipse(leakCx - leakR, leakCy - leakR, leakR * 2.0f, leakR * 2.0f);
		PathGradientBrush leakBrush(&leakPath);
		BYTE leakAlpha = (BYTE)std::clamp((int)((REAL)s.eightmm_light_leak_alpha * 255.0f * t), 0, 255);
		leakBrush.SetCenterColor(Color(leakAlpha, 255, 200, 120));
		Color leakSurround[] = {Color(0, 255, 200, 120)};
		INT leakSurroundCount = 1;
		leakBrush.SetSurroundColors(leakSurround, &leakSurroundCount);
		g.FillPath(&leakBrush, &leakPath);
	}

	{
		GraphicsPath vignettePath;
		vignettePath.AddEllipse(-cardW * 0.4f, -cardH * 0.4f, cardW * 1.8f, cardH * 1.8f);
		PathGradientBrush vignetteBrush(&vignettePath);
		vignetteBrush.SetCenterColor(Color(0, 0, 0, 0));
		BYTE vignetteAlpha = (BYTE)std::clamp((int)((REAL)s.eightmm_vignette_strength * 255.0f * t), 0, 255);
		Color vignetteSurround[] = {Color(vignetteAlpha, 0, 0, 0)};
		INT vignetteSurroundCount = 1;
		vignetteBrush.SetSurroundColors(vignetteSurround, &vignetteSurroundCount);
		g.FillRectangle(&vignetteBrush, Rect(0, 0, cardW, cardH));
	}

	Bitmap *noiseTex = EnsureNoiseTexture(ctx->eightmm_noise_texture);
	if (noiseTex) {
		ImageAttributes noiseAttr;
		Gdiplus::ColorMatrix cm = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, t * 0.7f, 0, 0, 0, 0, 0, 1};
		noiseAttr.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

		RectF texRect(0.0f, 0.0f, (REAL)VHS_NOISE_TEXTURE_SIZE, (REAL)VHS_NOISE_TEXTURE_SIZE);
		TextureBrush noiseBrush(noiseTex, texRect, &noiseAttr);
		noiseBrush.SetWrapMode(WrapModeTile);
		Matrix m;
		m.Translate((REAL)-ctx->eightmm_noise_offset_x, (REAL)-ctx->eightmm_noise_offset_y);
		noiseBrush.SetTransform(&m);
		g.FillRectangle(&noiseBrush, Rect(0, 0, cardW, cardH));
	}

	auto expectedCountRoll = [&](double expected) -> int {
		int whole = (int)std::floor(expected);
		double frac = expected - whole;
		std::uniform_real_distribution<double> d(0.0, 1.0);
		if (d(ctx->eightmm_rng) < frac)
			whole += 1;
		return whole;
	};

	int vScratchCount = expectedCountRoll((s.eightmm_scratch_intensity / 100.0) * (s.eightmm_scratch_max_count / 2.0));
	if (vScratchCount > 0) {
		std::uniform_int_distribution<int> xDist(0, std::max(0, cardW - 1));
		std::uniform_int_distribution<int> alphaDist(15, 55);
		for (int i = 0; i < vScratchCount; i++) {
			int x = xDist(ctx->eightmm_rng);
			int alpha = std::clamp((int)(alphaDist(ctx->eightmm_rng) * t), 0, 255);
			SolidBrush scratchBrush(Color(alpha, 210, 210, 200));
			g.FillRectangle(&scratchBrush, Rect(x, 0, 1, cardH));
		}
	}

	int hairCount = expectedCountRoll((s.eightmm_scratch_intensity / 100.0) * s.eightmm_scratch_max_count);

	std::uniform_real_distribution<double> startXDist(0.0, (double)cardW);
	std::uniform_real_distribution<double> startYDist(0.0, (double)cardH);
	std::uniform_real_distribution<double> angleDist(0.0, 6.28318530718);
	std::uniform_real_distribution<double> turnDist(-0.5, 0.5);
	std::uniform_real_distribution<double> stepLenDist(4.0, 10.0);
	std::uniform_int_distribution<int> segCountDist(14, 32);

	int coreAlpha = std::clamp((int)(150.0f * t), 0, 255);
	Pen glowPen(Color(coreAlpha / 4, 235, 225, 200), 3.0f);
	glowPen.SetLineJoin(LineJoinRound);
	glowPen.SetStartCap(LineCapRound);
	glowPen.SetEndCap(LineCapRound);

	Pen corePen(Color(coreAlpha, 245, 235, 210), 1.0f);
	corePen.SetLineJoin(LineJoinRound);
	corePen.SetStartCap(LineCapRound);
	corePen.SetEndCap(LineCapRound);

	for (int i = 0; i < hairCount; i++) {
		REAL x = (REAL)startXDist(ctx->eightmm_rng);
		REAL y = (REAL)startYDist(ctx->eightmm_rng);
		double angle = angleDist(ctx->eightmm_rng);
		int segCount = segCountDist(ctx->eightmm_rng);

		std::vector<PointF> pts;
		pts.reserve(segCount + 1);
		pts.push_back(PointF(x, y));
		for (int seg = 0; seg < segCount; seg++) {
			angle += turnDist(ctx->eightmm_rng);
			REAL stepLen = (REAL)stepLenDist(ctx->eightmm_rng);
			x += (REAL)std::cos(angle) * stepLen;
			y += (REAL)std::sin(angle) * stepLen;
			pts.push_back(PointF(x, y));
		}

		if (pts.size() < 4)
			continue;

		GraphicsPath hairPath;
		hairPath.AddCurve(pts.data(), (INT)pts.size(), 0.3f);

		g.DrawPath(&glowPen, &hairPath);
		g.DrawPath(&corePen, &hairPath);
	}

	int dustCount = expectedCountRoll((s.eightmm_dust_intensity / 100.0) * s.eightmm_dust_max_count);
	if (dustCount > 0) {
		std::uniform_int_distribution<int> dxDist(0, std::max(0, cardW - 1));
		std::uniform_int_distribution<int> dyDist(0, std::max(0, cardH - 1));
		std::uniform_int_distribution<int> dAlphaDist(15, 140);
		std::uniform_int_distribution<int> shadeDist(0, 9);
		std::uniform_int_distribution<int> shapeDist(0, 99);
		std::uniform_real_distribution<double> angleDist(0.0, 6.28318530718);
		std::uniform_real_distribution<double> flkLenDist(2.0, 7.0);
		std::uniform_int_distribution<int> blobPtCountDist(3, 5);
		std::uniform_real_distribution<double> blobRadDist(0.6, 2.2);
		std::uniform_real_distribution<double> blobJitterDist(-0.3, 0.3);
		std::uniform_int_distribution<int> clusterCountDist(2, 4);
		std::uniform_int_distribution<int> clusterSpreadDist(-3, 3);

		SolidBrush dustBrush(Color(0, 0, 0, 0));
		Pen dustPen(Color(0, 0, 0, 0), 1.0f);

		for (int i = 0; i < dustCount; i++) {
			int dx = dxDist(ctx->eightmm_rng);
			int dy = dyDist(ctx->eightmm_rng);
			int alpha = std::clamp((int)(dAlphaDist(ctx->eightmm_rng) * t), 0, 255);
			bool light = shadeDist(ctx->eightmm_rng) >= 2;
			Color c = light ? Color(alpha, 235, 228, 210) : Color(alpha, 10, 10, 10);

			int shapeRoll = shapeDist(ctx->eightmm_rng);
			if (shapeRoll < 50) {
				// plain speck -- the common case
				dustBrush.SetColor(c);
				g.FillRectangle(&dustBrush, Rect(dx, dy, 1, 1));
			} else if (shapeRoll < 72) {
				// short angled fleck/fiber
				dustPen.SetColor(c);
				double ang = angleDist(ctx->eightmm_rng);
				double len = flkLenDist(ctx->eightmm_rng);
				REAL ex = (REAL)(dx + std::cos(ang) * len);
				REAL ey = (REAL)(dy + std::sin(ang) * len);
				g.DrawLine(&dustPen, (REAL)dx, (REAL)dy, ex, ey);
			} else if (shapeRoll < 90) {
				// small irregular blob
				int nPts = blobPtCountDist(ctx->eightmm_rng);
				std::vector<PointF> blobPts;
				blobPts.reserve(nPts);
				for (int k = 0; k < nPts; k++) {
					double ang = (6.28318530718 * k / nPts) + blobJitterDist(ctx->eightmm_rng);
					double rad = blobRadDist(ctx->eightmm_rng);
					blobPts.push_back(PointF((REAL)(dx + std::cos(ang) * rad), (REAL)(dy + std::sin(ang) * rad)));
				}
				GraphicsPath blobPath;
				blobPath.AddPolygon(blobPts.data(), (INT)blobPts.size());
				dustBrush.SetColor(c);
				g.FillPath(&dustBrush, &blobPath);
			} else {
				// loose cluster of a few pixels
				dustBrush.SetColor(c);
				int clusterCount = clusterCountDist(ctx->eightmm_rng);
				for (int k = 0; k < clusterCount; k++) {
					int cx = dx + clusterSpreadDist(ctx->eightmm_rng);
					int cy = dy + clusterSpreadDist(ctx->eightmm_rng);
					g.FillRectangle(&dustBrush, Rect(cx, cy, 1, 1));
				}
			}
		}
	}

	g.SetClip(&savedClip);
}

static void DrawDuotoneOverlay(Bitmap &card, int cardW, int cardH, long long shadowColorPacked, long long highlightColorPacked, int intensity)
{
	REAL t = std::clamp(intensity, 0, 100) / 100.0f;
	if (t <= 0.0f || cardW <= 0 || cardH <= 0)
		return;

	Color shadow = ObsColorToGdip(shadowColorPacked);
	Color highlight = ObsColorToGdip(highlightColorPacked);

	BitmapData bd;
	Rect full(0, 0, cardW, cardH);
	if (card.LockBits(&full, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok)
		return;

	BYTE sr = shadow.GetR(), sg = shadow.GetG(), sb = shadow.GetB();
	BYTE hr = highlight.GetR(), hg = highlight.GetG(), hb = highlight.GetB();

	uint8_t *base = (uint8_t *)bd.Scan0;
	for (int y = 0; y < cardH; y++) {
		uint8_t *row = base + (size_t)y * bd.Stride;
		for (int x = 0; x < cardW; x++) {
			uint8_t *px = row + (size_t)x * 4;
			REAL lum = (0.299f * px[2] + 0.587f * px[1] + 0.114f * px[0]) / 255.0f;
			uint8_t mr = (uint8_t)(sr + (hr - sr) * lum);
			uint8_t mg = (uint8_t)(sg + (hg - sg) * lum);
			uint8_t mb = (uint8_t)(sb + (hb - sb) * lum);
			px[0] = (uint8_t)(px[0] + (mb - px[0]) * t);
			px[1] = (uint8_t)(px[1] + (mg - px[1]) * t);
			px[2] = (uint8_t)(px[2] + (mr - px[2]) * t);
		}
	}
	card.UnlockBits(&bd);
}

static void DrawBwOverlay(Graphics &g, Bitmap &card, GraphicsPath &clipPath, int cardW, int cardH, int desaturation, int contrast, double vignetteStrength)
{
	REAL desat = std::clamp(desaturation, 0, 100) / 100.0f;
	if (desat > 0.0f) {
		Gdiplus::HueSaturationLightness hsl;
		Gdiplus::HueSaturationLightnessParams hslParams = {0, (INT)(-100.0f * desat), 0};
		if (hsl.SetParameters(&hslParams) == Ok)
			card.ApplyEffect(&hsl, nullptr);
	}

	if (contrast != 0) {
		Gdiplus::BrightnessContrast bc;
		Gdiplus::BrightnessContrastParams bcParams = {0, std::clamp(contrast * 2, -255, 255)};
		if (bc.SetParameters(&bcParams) == Ok)
			card.ApplyEffect(&bc, nullptr);
	}

	if (vignetteStrength > 0.0 && cardW > 0 && cardH > 0) {
		Region savedClip;
		g.GetClip(&savedClip);
		g.SetClip(&clipPath);

		GraphicsPath vignettePath;
		vignettePath.AddEllipse(-cardW * 0.4f, -cardH * 0.4f, cardW * 1.8f, cardH * 1.8f);
		PathGradientBrush vignetteBrush(&vignettePath);
		vignetteBrush.SetCenterColor(Color(0, 0, 0, 0));
		BYTE vignetteAlpha = (BYTE)std::clamp((int)(vignetteStrength * 255.0), 0, 255);
		Color vignetteSurround[] = {Color(vignetteAlpha, 0, 0, 0)};
		INT vignetteSurroundCount = 1;
		vignetteBrush.SetSurroundColors(vignetteSurround, &vignetteSurroundCount);
		g.FillRectangle(&vignetteBrush, Rect(0, 0, cardW, cardH));

		g.SetClip(&savedClip);
	}
}

static void DrawGlitchOverlay(Graphics &g, Bitmap &card, spotify_source *ctx, GraphicsPath &clipPath, int cardW, int cardH, const AppearanceSettings &s)
{
	REAL t = std::clamp(s.glitch_intensity, 0, 100) / 100.0f;
	if (t <= 0.0f || cardW <= 0 || cardH <= 0)
		return;

	Region savedClip;
	g.GetClip(&savedClip);
	g.SetClip(&clipPath);

	if (!ctx->glitch_tears.empty() && cardH > 0) {
		std::unique_ptr<Bitmap> tearSnap(card.Clone(0, 0, cardW, cardH, PixelFormat32bppARGB));
		if (tearSnap) {
			for (const auto &tear : ctx->glitch_tears) {
				int h = std::clamp(tear.h, 1, cardH);
				int y = std::clamp(tear.y, 0, cardH - h);
				int srcY = std::clamp(tear.srcY, 0, cardH - h);
				REAL offsetPx = (REAL)tear.offsetX * t;
				RectF destBand(offsetPx, (REAL)y, (REAL)cardW, (REAL)h);
				g.DrawImage(tearSnap.get(), destBand, 0.0f, (REAL)srcY, (REAL)cardW, (REAL)h, UnitPixel, nullptr);
			}
		}
	}

	if (!ctx->glitch_channel_blocks.empty()) {
		BitmapData bd;
		Rect full(0, 0, cardW, cardH);
		if (card.LockBits(&full, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
			ctx->overlay_scratch_pixels.resize((size_t)cardW * cardH * 4);
			std::vector<uint8_t> &src = ctx->overlay_scratch_pixels;
			const uint8_t *srcRow = (const uint8_t *)bd.Scan0;
			for (int y = 0; y < cardH; y++)
				memcpy(src.data() + (size_t)y * cardW * 4, srcRow + (size_t)y * bd.Stride, (size_t)cardW * 4);
			card.UnlockBits(&bd);

			if (card.LockBits(&full, ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
				uint8_t *dstBase = (uint8_t *)bd.Scan0;
				for (const auto &block : ctx->glitch_channel_blocks) {
					int bw = std::clamp(block.w, 1, cardW);
					int bh = std::clamp(block.h, 1, cardH);
					int bx = std::clamp(block.x, 0, cardW - bw);
					int by = std::clamp(block.y, 0, cardH - bh);
					int channel = std::clamp(block.channel, 0, 2);
					int dx = (int)std::lround((double)block.offsetX * t);
					int dy = (int)std::lround((double)block.offsetY * t);

					for (int y = by; y < by + bh; y++) {
						uint8_t *row = dstBase + (size_t)y * bd.Stride;
						for (int x = bx; x < bx + bw; x++) {
							int sx = std::clamp(x + dx, 0, cardW - 1);
							int sy = std::clamp(y + dy, 0, cardH - 1);
							size_t srcIdx = ((size_t)sy * cardW + (size_t)sx) * 4;
							row[(size_t)x * 4 + (size_t)channel] = src[srcIdx + (size_t)channel];
						}
					}
				}
				card.UnlockBits(&bd);
			}
		}
	}

	if (!ctx->glitch_sort_rows.empty()) {
		BitmapData bd;
		Rect full(0, 0, cardW, cardH);
		if (card.LockBits(&full, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
			uint8_t *base = (uint8_t *)bd.Scan0;
			REAL halfBand = std::clamp(s.glitch_pixel_sort_threshold, 0, 100) / 100.0f * 127.0f;

			auto luminance = [](const uint8_t *px) -> REAL {
				return 0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2];
			};

			for (const auto &sortRow : ctx->glitch_sort_rows) {
				int y = std::clamp(sortRow.y, 0, cardH - 1);
				uint8_t *row = base + (size_t)y * bd.Stride;

				REAL lower = std::clamp(sortRow.center - halfBand, 0.0f, 255.0f);
				REAL upper = std::clamp(sortRow.center + halfBand, 0.0f, 255.0f);

				int x = 0;
				while (x < cardW) {
					if (luminance(row + (size_t)x * 4) < lower || luminance(row + (size_t)x * 4) > upper) {
						x++;
						continue;
					}

					int runStart = x;
					while (x < cardW) {
						REAL lum = luminance(row + (size_t)x * 4);
						if (lum < lower || lum > upper)
							break;
						x++;
					}
					int runEnd = x; // exclusive

					if (runEnd - runStart >= 2) {
						std::vector<std::array<uint8_t, 4>> &px = ctx->glitch_sort_scratch;
						px.clear();
						px.reserve(runEnd - runStart);
						for (int i = runStart; i < runEnd; i++) {
							uint8_t *p = row + (size_t)i * 4;
							px.push_back({p[0], p[1], p[2], p[3]});
						}
						std::sort(px.begin(), px.end(), [&](const std::array<uint8_t, 4> &a, const std::array<uint8_t, 4> &b) { return luminance(a.data()) < luminance(b.data()); });
						for (int i = 0; i < (int)px.size(); i++) {
							uint8_t *p = row + (size_t)(runStart + i) * 4;
							p[0] = px[(size_t)i][0];
							p[1] = px[(size_t)i][1];
							p[2] = px[(size_t)i][2];
							p[3] = px[(size_t)i][3];
						}
					}
				}
			}
			card.UnlockBits(&bd);
		}
	}

	g.SetClip(&savedClip);
}

static void compose_bitmap_impl(spotify_source *ctx, const std::string &title, const std::string &artist, const AppearanceSettings &s)
{
	const int cardW = std::max(s.card_w, 50);
	const int cardH = std::max(s.card_h, 30);

	if (!ctx->cached_bitmap || ctx->cached_bitmap_w != cardW || ctx->cached_bitmap_h != cardH) {
		ctx->cached_bitmap = std::make_unique<Bitmap>(cardW, cardH, PixelFormat32bppARGB);
		ctx->cached_bitmap_w = cardW;
		ctx->cached_bitmap_h = cardH;
	}
	Bitmap &card = *ctx->cached_bitmap;
	Graphics g(&card);

	g.SetSmoothingMode(SmoothingModeHighQuality);
	g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
	g.SetCompositingQuality(CompositingQualityHighQuality);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(Color(0, 0, 0, 0));

	// card background
	GraphicsPath bgPath;
	AddRoundedRect(bgPath, Rect(0, 0, cardW, cardH), s.background_corner_radius);

	Image *bgImage = nullptr;
	if (s.use_album_art_as_bg && ctx->cached_art_image) {
		DrawAlbumArtBackground(g, ctx, ctx->cached_art_image.get(), bgPath, cardW, cardH, s.album_art_bg_blur_pct, s.bg_opacity);
	} else {
		if (s.use_bg_image) {
			if (!ctx->cached_bg_image || ctx->settings_dirty)
				bgImage = EnsureBackgroundImage(ctx, s.bg_image_path);
			else
				bgImage = ctx->cached_bg_image.get();
		}
		if (bgImage) {
			Region savedBgClip;
			g.GetClip(&savedBgClip);
			g.SetClip(&bgPath);

			UINT imgW = bgImage->GetWidth();
			UINT imgH = bgImage->GetHeight();
			REAL srcW = (REAL)std::min<UINT>(imgW, (UINT)cardW);
			REAL srcH = (REAL)std::min<UINT>(imgH, (UINT)cardH);

			ImageAttributes bgAttr;
			if (s.bg_opacity < 100) {
				REAL a = std::clamp(s.bg_opacity, 0, 100) / 100.0f;
				Gdiplus::ColorMatrix cm = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, 1};
				bgAttr.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
			}

			// Draw the top-left crop of the source image 1:1 (no scaling) into the card
			RectF bgDestRect(0.0f, 0.0f, srcW, srcH);
			g.DrawImage(bgImage, bgDestRect, 0.0f, 0.0f, srcW, srcH, UnitPixel, &bgAttr);

			g.SetClip(&savedBgClip);
		} else {
			SolidBrush bgBrush(ObsColorToGdipWithAlpha(s.bg_color, s.bg_opacity));
			g.FillPath(&bgBrush, &bgPath);
		}
	}

	int titleSize = s.title_font_size > 0 ? s.title_font_size : DEFAULT_TITLE_FONT_SIZE;
	int artistSize = s.artist_font_size > 0 ? s.artist_font_size : DEFAULT_ARTIST_FONT_SIZE;

	Font &titleFont = *EnsureFont(ctx->title_font_cache, s.title_font_face, s.title_font_style, titleSize, s.title_font_flags);
	Font &artistFont = *EnsureFont(ctx->artist_font_cache, s.artist_font_face, s.artist_font_style, artistSize, s.artist_font_flags);

	Color titleColor = ObsColorToGdip(s.title_color);
	Color artistColor = ObsColorToGdip(s.artist_color);
	SolidBrush titleBrush(titleColor);
	SolidBrush artistBrush(artistColor);

	Color titleOutlineColor = ObsColorToGdip(s.title_outline_color);
	Color artistOutlineColor = ObsColorToGdip(s.artist_outline_color);

	int titleLineH = titleSize + 10;
	int artistLineH = artistSize + 8;
	int progressH = s.show_progress_bar ? (s.progress_bar_gap + s.progress_bar_height) : 0;
	int blockH = titleLineH + artistLineH + progressH;

	int artSize = 0;
	Rect artRect;
	RectF titleRect, artistRect;
	bool centerText = false;
	Rect vuBlockRect(0, 0, 0, 0);
	Rect progressBarRect(0, 0, 0, 0);

	bool showArt = !s.hide_album_art;

	if (s.vertical_layout) {
		constexpr int GAP_ART_TEXT = 14;
		constexpr int GAP_TEXT_VU = VU_GAP_BEFORE_TEXT;

		int textW = cardW - PAD * 2;
		if (textW < MIN_TEXT_W)
			textW = MIN_TEXT_W;
		int textX = PAD;
		int textTop;

		if (showArt) {
			int maxArtByWidth = cardW - PAD * 2;
			if (maxArtByWidth < MIN_ART_SIZE)
				maxArtByWidth = MIN_ART_SIZE;

			int reservedNonArt = PAD * 2 + GAP_ART_TEXT + blockH + (s.vu_meter_enabled ? (GAP_TEXT_VU + s.vu_height) : 0);
			artSize = cardH - reservedNonArt;
			if (artSize > maxArtByWidth)
				artSize = maxArtByWidth;
			if (artSize < MIN_ART_SIZE)
				artSize = MIN_ART_SIZE;

			int artX = (cardW - artSize) / 2;
			int artY = PAD;
			artRect = Rect(artX, artY, artSize, artSize);

			textTop = artY + artSize + GAP_ART_TEXT + s.text_offset_y;
		} else {
			artSize = 0;
			artRect = Rect(0, 0, 0, 0);
			textTop = PAD + s.text_offset_y;
		}

		titleRect = RectF((REAL)textX, (REAL)textTop, (REAL)textW, (REAL)titleLineH);
		artistRect = RectF((REAL)textX, (REAL)(textTop + titleLineH), (REAL)textW, (REAL)artistLineH);

		if (s.show_progress_bar) {
			int progressY = textTop + titleLineH + artistLineH + s.progress_bar_gap;
			progressBarRect = Rect(textX, progressY, textW, s.progress_bar_height);
		}

		if (s.vu_meter_enabled) {
			int vuTop = textTop + blockH + GAP_TEXT_VU;
			int vuLeft = (cardW - s.vu_width) / 2;
			vuBlockRect = Rect(vuLeft, vuTop, s.vu_width, s.vu_height);
		}

		centerText = true;
	} else {
		int textX;

		if (showArt) {
			artSize = cardH - PAD * 2;
			if (artSize < MIN_ART_SIZE)
				artSize = MIN_ART_SIZE;
			int maxArtForWidth = cardW - PAD * 2 - MIN_TEXT_W;
			if (artSize > maxArtForWidth)
				artSize = std::max(MIN_ART_SIZE, maxArtForWidth);

			artRect = Rect(PAD, PAD, artSize, artSize);
			textX = PAD + artSize + 14;
		} else {
			artSize = 0;
			artRect = Rect(0, 0, 0, 0);
			textX = PAD;
		}

		int vuBlockWidthReserved = s.vu_meter_enabled ? (s.vu_width + VU_GAP_BEFORE_TEXT) : 0;
		int textW = cardW - textX - PAD - vuBlockWidthReserved;
		if (textW < MIN_TEXT_W)
			textW = MIN_TEXT_W;

		int topY = (cardH - blockH) / 2 + s.text_offset_y;
		titleRect = RectF((REAL)textX, (REAL)topY, (REAL)textW, (REAL)titleLineH);
		artistRect = RectF((REAL)textX, (REAL)(topY + titleLineH), (REAL)textW, (REAL)artistLineH);

		if (s.show_progress_bar) {
			int progressY = topY + titleLineH + artistLineH + s.progress_bar_gap;
			progressBarRect = Rect(textX, progressY, textW, s.progress_bar_height);
		}

		if (s.vu_meter_enabled) {
			int vuRight = cardW - PAD;
			int vuLeft = vuRight - s.vu_width;
			int vuTop = (cardH - s.vu_height) / 2;
			vuBlockRect = Rect(vuLeft, vuTop, s.vu_width, s.vu_height);
		}

		centerText = false;
	}

	if (showArt) {
		GraphicsPath artClip;
		AddRoundedRect(artClip, artRect, s.album_art_corner_radius);

		Region savedClip;
		g.GetClip(&savedClip);
		g.SetClip(&artClip);

		bool drewArt = false;
		if (ctx->cached_art_image) {
			g.DrawImage(ctx->cached_art_image.get(), artRect);
			drewArt = true;
		}
		if (!drewArt && s.show_goat_placeholder) {
			Image *goat = GetGoatImage(ctx);
			if (goat) {
				g.DrawImage(goat, artRect);
				drewArt = true;
			}
		}
		if (!drewArt) {
			SolidBrush placeholder(Color(255, 55, 55, 60));
			g.FillRectangle(&placeholder, artRect);
		}
		g.SetClip(&savedClip);
	}

	// text (shared drawing code)
	static const std::string kAttributionTitle = "NowPlayingWidget by lingeriegoat";
	static const std::string kAttributionArtist = "Play some music to get started";
	bool useAttribution = !ctx->have_track && s.show_plugin_attribution;
	const std::string &displayTitle = useAttribution ? kAttributionTitle : title;
	const std::string &displayArtist = useAttribution ? kAttributionArtist : artist;

	if (ctx->cached_wtitle_src != displayTitle) {
		ctx->cached_wtitle_src = displayTitle;
		ctx->cached_wtitle = Utf8ToWide(displayTitle);
	}
	if (ctx->cached_wartist_src != displayArtist) {
		ctx->cached_wartist_src = displayArtist;
		ctx->cached_wartist = Utf8ToWide(displayArtist);
	}
	const std::wstring &wtitle = ctx->cached_wtitle;
	const std::wstring &wartist = ctx->cached_wartist;

	bool titleScroll = false, artistScroll = false;
	double titleAvgChar = ctx->title_avg_char_px, artistAvgChar = ctx->artist_avg_char_px;
	double titleMaxOffset = ctx->title_scroll_max_px, artistMaxOffset = ctx->artist_scroll_max_px;
	DrawScrollableLine(g, wtitle, titleFont, titleBrush, titleRect, ctx->title_scroll_px, centerText, s.title_outline_enabled, (float)s.title_outline_size, titleOutlineColor, &titleScroll, &titleAvgChar, &titleMaxOffset, &ctx->title_measure_cache);
	DrawScrollableLine(g, wartist, artistFont, artistBrush, artistRect, ctx->artist_scroll_px, centerText, s.artist_outline_enabled, (float)s.artist_outline_size, artistOutlineColor, &artistScroll, &artistAvgChar, &artistMaxOffset, &ctx->artist_measure_cache);
	ctx->title_needs_scroll = titleScroll;
	ctx->artist_needs_scroll = artistScroll;
	ctx->title_avg_char_px = titleAvgChar;
	ctx->artist_avg_char_px = artistAvgChar;
	ctx->title_scroll_max_px = titleMaxOffset;
	ctx->artist_scroll_max_px = artistMaxOffset;

	DrawVuMeter(g, ctx, s, vuBlockRect);

	DrawProgressBar(g, ctx, s, progressBarRect);

	if (s.card_style == "vhs")
		DrawVhsOverlay(g, card, ctx, bgPath, cardW, cardH, s);
	else if (s.card_style == "8mm")
		DrawEightMmOverlay(g, card, ctx, bgPath, cardW, cardH, s);
	else if (s.card_style == "duotone")
		DrawDuotoneOverlay(card, cardW, cardH, s.duotone_shadow_color, s.duotone_highlight_color, s.duotone_intensity);
	else if (s.card_style == "bw")
		DrawBwOverlay(g, card, bgPath, cardW, cardH, s.bw_desaturation, s.bw_contrast, s.bw_vignette_strength);
	else if (s.card_style == "glitch")
		DrawGlitchOverlay(g, card, ctx, bgPath, cardW, cardH, s);

	BitmapData bd;
	Rect full(0, 0, cardW, cardH);
	if (card.LockBits(&full, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
		return;

	std::vector<uint8_t> buf((size_t)cardW * cardH * 4);
	const uint8_t *src = (const uint8_t *)bd.Scan0;
	for (int y = 0; y < cardH; y++)
		memcpy(buf.data() + (size_t)y * cardW * 4, src + (size_t)y * bd.Stride, (size_t)cardW * 4);
	card.UnlockBits(&bd);

	ScaleAlphaChannel(buf, ctx->autohide_alpha);

	{
		std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
		ctx->pending_pixels = std::move(buf);
		ctx->pending_w = (uint32_t)cardW;
		ctx->pending_h = (uint32_t)cardH;
	}
	ctx->new_bitmap_ready = true;
}

static void compose_bitmap(spotify_source *ctx, const std::string &title, const std::string &artist, const AppearanceSettings &s)
{
	__try {
		compose_bitmap_impl(ctx, title, artist, s);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) {
			_resetstkoflw();
		}
		blog(LOG_ERROR, "[spotify_now_playing] compose_bitmap failed: structured exception 0x%08lX", GetExceptionCode());
	}
}

static AppearanceSettings snapshot_settings(spotify_source *ctx)
{
	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	AppearanceSettings s;
#define X(type, name, def) s.name = ctx->name;
	APPEARANCE_SETTINGS_FIELDS(X)
#undef X
	return s;
}

static bool UpdateAutohideAlpha(spotify_source *ctx, const AppearanceSettings &s, std::chrono::steady_clock::time_point now)
{
	double dtMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->last_autohide_tick).count();
	ctx->last_autohide_tick = now;
	if (dtMs <= 0.0 || dtMs > 2000.0) // first call, or a long gap since the last tick (e.g. the card was hidden)
		dtMs = 50.0;

	float target = 1.0f;

	if (s.autohide_enabled) {
		if (obs_source_active(ctx->source)) {
			double elapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->autohide_reference_time).count();
			double delayMs = (double)std::max(0, s.autohide_after_s) * 1000.0;
			if (elapsedMs >= delayMs)
				target = 0.0f;
		}
	}

	if (s.autohide_when_not_playing) {
		double notPlayingElapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->last_playing_time).count();
		double notPlayingDelayMs = (double)DEFAULT_NOT_PLAYING_AUTOHIDE_AFTER_S * 1000.0;
		if (notPlayingElapsedMs >= notPlayingDelayMs)
			target = 0.0f;
	}

	if (ctx->autohide_alpha == target)
		return false;

	double step = dtMs / (double)AUTOHIDE_FADE_MS;
	if (target > ctx->autohide_alpha)
		ctx->autohide_alpha = (float)std::min((double)target, (double)ctx->autohide_alpha + step);
	else
		ctx->autohide_alpha = (float)std::max((double)target, (double)ctx->autohide_alpha - step);

	return true;
}

static void poll_loop(spotify_source *ctx)
{
	bool com_initialized = false;
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		com_initialized = true;
	} catch (const winrt::hresult_error &) {
		com_initialized = false;
	} catch (...) {
		com_initialized = false;
	}

	//manager must be created inside poll_loop so OBS exits cleanly. Its a com apartment context thing.
	GlobalSystemMediaTransportControlsSessionManager sessionManager = nullptr;

	//Hold the last good bitmap for 2 seconds as some clients drop their session during track skip
	constexpr auto MISSING_SESSION_GRACE = std::chrono::seconds(DEFAULT_SESSION_GRACE_SECONDS);
	bool gap_active = false;
	std::chrono::steady_clock::time_point gap_start{};

	while (ctx->running) {
		try {
			if (!ctx->is_active) {
				if (ctx->settings_dirty) {
					compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
					ctx->settings_dirty = false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			if (!sessionManager && com_initialized) {
				try {
					sessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				} catch (const winrt::hresult_error &) {
					sessionManager = nullptr;
				} catch (...) {
					sessionManager = nullptr;
				}
			}

			NativeMediaInfo info{};
			bool has = GetCurrentTrackSafe(sessionManager, &info, ctx->kPossibleMusicSystems, ctx->kPossibleBrowserMediaSources, ctx->browser_media_source_enabled);

			std::string title = has ? std::string(info.SongName) : std::string();
			std::string artist = has ? std::string(info.ArtistName) : std::string();

			bool show_album_name;
			{
				std::lock_guard<std::mutex> lock(ctx->settings_mutex);
				show_album_name = ctx->show_album_name;
			}
			if (show_album_name) {
				std::string albumName = " - " + std::string(info.AlbumName);
				artist.append(albumName);
			}

			bool track_changed = (has != ctx->have_track) || (title != ctx->last_song) || (artist != ctx->last_artist);

			if (has) {
				gap_active = false;
				ctx->is_playing = info.IsPlaying;
				auto now = std::chrono::steady_clock::now();

				if (ctx->is_playing) {
					ctx->last_playing_time = now;
				}

				bool positionChanged = (info.CurrentPlaybackTimeTicks != ctx->playback_position_ticks) || (info.SongDurationTicks != ctx->song_duration_ticks);
				if (track_changed || positionChanged) {
					ctx->song_duration_ticks = info.SongDurationTicks;
					ctx->playback_position_ticks = info.CurrentPlaybackTimeTicks;
					ctx->position_sample_time = now;
				}

				if (track_changed) {
					UpdateCachedArt(ctx, info.ImageData, info.ImageLength);

					ctx->last_song = title;
					ctx->last_artist = artist;
					ctx->have_track = true;
					ctx->max_displayed_position_ticks = 0;
					ctx->autohide_reference_time = now;

					ctx->title_scroll_px = 0.0; // New track -- restart the marquee from the beginning.
					ctx->artist_scroll_px = 0.0;
					ctx->title_scroll_paused_at_end = false;
					ctx->artist_scroll_paused_at_end = false;
					ctx->title_scroll_paused_at_start = true;
					ctx->artist_scroll_paused_at_start = true;
					ctx->title_pause_start = now;
					ctx->artist_pause_start = now;
					ctx->last_scroll_tick = std::chrono::steady_clock::now();

					AppearanceSettings snap = snapshot_settings(ctx);

					std::vector<uint8_t> fromPixels;
					uint32_t fromW = 0, fromH = 0;
					{
						std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
						fromPixels = ctx->pending_pixels;
						fromW = ctx->pending_w;
						fromH = ctx->pending_h;
					}

					compose_bitmap(ctx, title, artist, snap);

					if (snap.track_change_animation_enabled && !fromPixels.empty()) {
						std::vector<uint8_t> toPixels;
						uint32_t toW = 0, toH = 0;
						{
							std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
							toPixels = ctx->pending_pixels;
							toW = ctx->pending_w;
							toH = ctx->pending_h;
						}

						if (fromW == toW && fromH == toH) {
							ctx->transition_from_pixels = std::move(fromPixels);
							ctx->transition_to_pixels = std::move(toPixels);
							ctx->transition_w = toW;
							ctx->transition_h = toH;
							ctx->transition_start = now;
							ctx->transition_active = true;

							std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
							ctx->pending_pixels = ctx->transition_from_pixels;
							ctx->pending_w = fromW;
							ctx->pending_h = fromH;
							ctx->new_bitmap_ready = true;
						}
					}
				} else if (ArtBytesDiffer(ctx->last_art_bytes, info.ImageData, info.ImageLength)) {
					UpdateCachedArt(ctx, info.ImageData, info.ImageLength);
					compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
				} else if (ctx->settings_dirty) {
					ctx->title_scroll_px = 0.0;
					ctx->artist_scroll_px = 0.0;
					ctx->title_scroll_paused_at_end = false;
					ctx->artist_scroll_paused_at_end = false;
					ctx->title_scroll_paused_at_start = true;
					ctx->artist_scroll_paused_at_start = true;
					ctx->title_pause_start = now;
					ctx->artist_pause_start = now;
					compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
				}
			} else if (ctx->have_track) {
				ctx->is_playing = false;

				if (!gap_active) {
					gap_active = true;
					gap_start = std::chrono::steady_clock::now();
				}

				if (std::chrono::steady_clock::now() - gap_start >= MISSING_SESSION_GRACE) {
					ctx->last_song.clear();
					ctx->last_artist.clear();
					UpdateCachedArt(ctx, nullptr, 0);
					ctx->song_duration_ticks = 0;
					ctx->playback_position_ticks = 0;
					ctx->max_displayed_position_ticks = 0;
					ctx->have_track = false;
					ctx->autohide_reference_time = std::chrono::steady_clock::now();
					gap_active = false;
					ctx->title_scroll_px = 0.0;
					ctx->artist_scroll_px = 0.0;
					ctx->title_scroll_paused_at_end = false;
					ctx->artist_scroll_paused_at_end = false;
					ctx->title_scroll_paused_at_start = true;
					ctx->artist_scroll_paused_at_start = true;
					ctx->title_pause_start = std::chrono::steady_clock::now();
					ctx->artist_pause_start = std::chrono::steady_clock::now();
					compose_bitmap(ctx, "", "", snapshot_settings(ctx));
				}
			} else if (ctx->settings_dirty) {
				compose_bitmap(ctx, ctx->last_song, ctx->last_artist, snapshot_settings(ctx));
			}
			ctx->settings_dirty = false;

			if (has && info.ImageData != nullptr)
				FreeImageBuffer(info.ImageData);

			AppearanceSettings s = snapshot_settings(ctx);

			for (int waited = 0; waited < POLL_INTERVAL_MS && ctx->running; waited += 50) {
				if (ctx->settings_dirty)
					break; // let the outer loop apply the appearance change immediately

				if (ctx->have_track || s.show_plugin_attribution || s.autohide_when_not_playing) {
					auto now = std::chrono::steady_clock::now();

					bool autohideChanged = UpdateAutohideAlpha(ctx, s, now);

					if (ctx->transition_active) {
						double elapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->transition_start).count();
						double t = elapsedMs / (double)TRACK_CHANGE_TRANSITION_MS;

						std::vector<uint8_t> blended;
						BlendPixelBuffers(ctx->transition_from_pixels, ctx->transition_to_pixels, blended, t);
						ScaleAlphaChannel(blended, ctx->autohide_alpha);

						{
							std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
							ctx->pending_pixels = std::move(blended);
							ctx->pending_w = ctx->transition_w;
							ctx->pending_h = ctx->transition_h;
						}
						ctx->new_bitmap_ready = true;

						if (t >= 1.0) {
							ctx->transition_active = false;
							ctx->transition_from_pixels.clear();
							ctx->transition_to_pixels.clear();
						}
					} else {
						bool needCompose = autohideChanged;

						if (ctx->title_needs_scroll || ctx->artist_needs_scroll) {
							if (now - ctx->last_scroll_tick >= std::chrono::milliseconds(s.scroll_speed_ms)) {
								ctx->last_scroll_tick = now;

								if (ctx->title_needs_scroll) {
									if (ctx->title_scroll_paused_at_end || ctx->title_scroll_paused_at_start) {
										if (now - ctx->title_pause_start >= SCROLL_END_PAUSE) {
											if (ctx->title_scroll_paused_at_end) {
												ctx->title_scroll_px = 0.0;
												ctx->title_scroll_paused_at_end = false;
												ctx->title_scroll_paused_at_start = true;
												ctx->title_pause_start = now;
												needCompose = true;
											} else {
												ctx->title_scroll_paused_at_start = false;
												needCompose = true;
											}
										}
									} else {
										ctx->title_scroll_px += ctx->title_avg_char_px;
										if (ctx->title_scroll_px >= ctx->title_scroll_max_px) {
											ctx->title_scroll_px = ctx->title_scroll_max_px;
											ctx->title_scroll_paused_at_end = true;
											ctx->title_pause_start = now;
										}
										needCompose = true;
									}
								}

								if (ctx->artist_needs_scroll) {
									if (ctx->artist_scroll_paused_at_end || ctx->artist_scroll_paused_at_start) {
										if (now - ctx->artist_pause_start >= SCROLL_END_PAUSE) {
											if (ctx->artist_scroll_paused_at_end) {
												ctx->artist_scroll_px = 0.0;
												ctx->artist_scroll_paused_at_end = false;
												ctx->artist_scroll_paused_at_start = true;
												ctx->artist_pause_start = now;
												needCompose = true;
											} else {
												ctx->artist_scroll_paused_at_start = false;
												needCompose = true;
											}
										}
									} else {
										ctx->artist_scroll_px += ctx->artist_avg_char_px;
										if (ctx->artist_scroll_px >= ctx->artist_scroll_max_px) {
											ctx->artist_scroll_px = ctx->artist_scroll_max_px;
											ctx->artist_scroll_paused_at_end = true;
											ctx->artist_pause_start = now;
										}
										needCompose = true;
									}
								}
							}
						}

						if (s.vu_meter_enabled && now - ctx->last_vu_tick >= std::chrono::milliseconds(s.vu_update_ms)) {
							ctx->last_vu_tick = now;
							int barCount = std::clamp(s.vu_bar_count, 1, VU_MAX_BAR_COUNT);
							if (ctx->is_playing) {
								std::uniform_real_distribution<double> dist(0.0, 1.0);

								double pull = std::clamp(s.vu_randomness, 0, 100) / 100.0;
								for (int i = 0; i < barCount; i++) {
									double target = dist(ctx->vu_rng);
									ctx->vu_bar_frac[i] += (target - ctx->vu_bar_frac[i]) * pull;
								}
								ctx->vu_was_playing = true;
								needCompose = true;
							} else if (ctx->vu_was_playing) {
								for (int i = 0; i < barCount; i++)
									ctx->vu_bar_frac[i] = 0.0;
								ctx->vu_was_playing = false;
								needCompose = true;
							}
						}

						if (s.show_progress_bar && ctx->have_track && now - ctx->last_progress_tick >= std::chrono::milliseconds(PROGRESS_UPDATE_MS)) {
							ctx->last_progress_tick = now;
							needCompose = true;
						}

						if (s.card_style == "vhs" && now - ctx->last_vhs_tick >= std::chrono::milliseconds(DEFAULT_ANIMATION_UPDATE_MS)) {
							ctx->last_vhs_tick = now;

							std::uniform_int_distribution<int> noiseOffDist(0, VHS_NOISE_TEXTURE_SIZE - 1);
							ctx->vhs_noise_offset_x = noiseOffDist(ctx->vhs_rng);
							ctx->vhs_noise_offset_y = noiseOffDist(ctx->vhs_rng);

							int scanlineSpacing = std::max(1, s.vhs_scanline_spacing);
							ctx->vhs_scanline_offset = (ctx->vhs_scanline_offset + 1) % scanlineSpacing;

							ctx->vhs_smear_phase = std::fmod(ctx->vhs_smear_phase + (float)VHS_SMEAR_PHASE_SPEED, 100000.0f);

							std::uniform_real_distribution<double> glitchChance(0.0, 1.0);
							ctx->vhs_glitch_active = glitchChance(ctx->vhs_rng) < (s.vhs_glitch_chance_pct / 100.0);
							if (ctx->vhs_glitch_active) {
								std::uniform_int_distribution<int> yDist(0, std::max(1, s.card_h - 1));
								ctx->vhs_glitch_y = yDist(ctx->vhs_rng);
							}

							std::uniform_real_distribution<double> jitterDist(-1.0, 1.0);

							int trackingMinMs = std::max(1, s.vhs_tracking_min_interval_s) * 1000;
							int trackingMaxMs = std::max(trackingMinMs, s.vhs_tracking_max_interval_s * 1000);
							std::uniform_int_distribution<int> intervalDist(trackingMinMs, trackingMaxMs);
							if (!ctx->vhs_tracking_active) {
								if (ctx->vhs_tracking_next_start == std::chrono::steady_clock::time_point{}) {
									ctx->vhs_tracking_next_start = now + std::chrono::milliseconds(intervalDist(ctx->vhs_rng));
								} else if (now >= ctx->vhs_tracking_next_start) {
									ctx->vhs_tracking_active = true;
									ctx->vhs_tracking_line_y = 0;
									int lineMin = std::clamp(s.vhs_tracking_line_min_count, 1, VHS_TRACKING_LINE_ARRAY_CAP);
									int lineMax = std::clamp(s.vhs_tracking_line_max_count, lineMin, VHS_TRACKING_LINE_ARRAY_CAP);
									std::uniform_int_distribution<int> countDist(lineMin, lineMax);
									ctx->vhs_tracking_line_count = countDist(ctx->vhs_rng);
									for (int i = 0; i < VHS_TRACKING_LINE_ARRAY_CAP; i++)
										ctx->vhs_tracking_shift_dir[i] = (float)jitterDist(ctx->vhs_rng);
								}
							} else {
								for (int i = 0; i < VHS_TRACKING_LINE_ARRAY_CAP; i++)
									ctx->vhs_tracking_shift_dir[i] = (float)jitterDist(ctx->vhs_rng);

								ctx->vhs_tracking_line_y += std::max(1, s.card_h / 24);
								if (ctx->vhs_tracking_line_y >= s.card_h + ctx->vhs_tracking_line_count * std::max(0, s.vhs_tracking_line_gap)) {
									ctx->vhs_tracking_active = false;
									ctx->vhs_tracking_next_start = now + std::chrono::milliseconds(intervalDist(ctx->vhs_rng));
								}
							}

							if (s.vhs_smear_amount <= 0) {
								ctx->vhs_smear_active = false;
								ctx->vhs_smear_next_start = std::chrono::steady_clock::time_point{};
							} else if (!ctx->vhs_smear_active) {
								if (ctx->vhs_smear_next_start == std::chrono::steady_clock::time_point{}) {
									std::uniform_real_distribution<double> smearGapDist(ctx->vhs_smear_min_interval_s, ctx->vhs_smear_max_interval_s);
									ctx->vhs_smear_next_start = now + std::chrono::milliseconds((int)(smearGapDist(ctx->vhs_rng) * 1000.0));
								} else if (now >= ctx->vhs_smear_next_start) {
									int cardHpx = std::max(1, s.card_h);
									int minH = std::min(cardHpx, std::max((int)std::lround(VHS_SMEAR_BAND_MIN_PX), (int)std::lround(cardHpx * VHS_SMEAR_BAND_MIN_FRAC)));
									int maxH = std::min(cardHpx, std::max(minH, (int)std::lround(cardHpx * VHS_SMEAR_BAND_MAX_FRAC)));
									std::uniform_int_distribution<int> bandHDist(minH, maxH);
									int bandH = bandHDist(ctx->vhs_rng);
									std::uniform_int_distribution<int> bandYDist(0, std::max(0, cardHpx - bandH));
									ctx->vhs_smear_band_y = bandYDist(ctx->vhs_rng);
									ctx->vhs_smear_band_h = bandH;
									ctx->vhs_smear_active = true;
									ctx->vhs_smear_burst_start = now;
									std::uniform_real_distribution<double> smearBurstDist(ctx->vhs_smear_burst_min_s, ctx->vhs_smear_burst_max_s);
									auto fadeInMs = std::chrono::milliseconds((int)(VHS_SMEAR_FADE_IN_S * 1000.0));
									auto fadeOutMs = std::chrono::milliseconds((int)(VHS_SMEAR_FADE_OUT_S * 1000.0));
									auto plateauMs = std::chrono::milliseconds((int)(smearBurstDist(ctx->vhs_rng) * 1000.0));
									ctx->vhs_smear_plateau_end = now + fadeInMs + plateauMs;
									ctx->vhs_smear_burst_end = ctx->vhs_smear_plateau_end + fadeOutMs;
								}
							} else if (now >= ctx->vhs_smear_burst_end) {
								ctx->vhs_smear_active = false;
								std::uniform_real_distribution<double> smearGapDist(ctx->vhs_smear_min_interval_s, ctx->vhs_smear_max_interval_s);
								ctx->vhs_smear_next_start = now + std::chrono::milliseconds((int)(smearGapDist(ctx->vhs_rng) * 1000.0));
							}

							needCompose = true;
						}

						if (s.card_style == "8mm" && now - ctx->last_eightmm_tick >= std::chrono::milliseconds(DEFAULT_ANIMATION_UPDATE_MS)) {
							ctx->last_eightmm_tick = now;

							std::uniform_int_distribution<int> noiseOffDist(0, VHS_NOISE_TEXTURE_SIZE - 1);
							ctx->eightmm_noise_offset_x = noiseOffDist(ctx->eightmm_rng);
							ctx->eightmm_noise_offset_y = noiseOffDist(ctx->eightmm_rng);

							std::uniform_real_distribution<double> weaveDist(-1.0, 1.0);
							ctx->eightmm_weave_offset = (float)(weaveDist(ctx->eightmm_rng) * s.eightmm_weave_px);

							std::uniform_real_distribution<double> flickerDist(-1.0, 1.0);
							ctx->eightmm_flicker_offset = (float)flickerDist(ctx->eightmm_rng);

							needCompose = true;
						}

						if (s.card_style == "glitch" && now - ctx->last_glitch_tick >= std::chrono::milliseconds(DEFAULT_ANIMATION_UPDATE_MS)) {
							ctx->last_glitch_tick = now;

							std::uniform_real_distribution<double> chanceDist(0.0, 1.0);

							ctx->glitch_sort_rows.clear();
							if (chanceDist(ctx->glitch_rng) < (s.glitch_pixel_sort_chance / 100.0)) {
								std::uniform_int_distribution<int> rowCountDist(1, std::max(1, s.glitch_pixel_sort_max_rows));
								int rowCount = rowCountDist(ctx->glitch_rng);
								std::uniform_int_distribution<int> yDist(0, std::max(0, s.card_h - 1));
								std::uniform_real_distribution<double> centerDist(0.0, 255.0);
								for (int i = 0; i < rowCount; i++) {
									GlitchSortRow row;
									row.y = yDist(ctx->glitch_rng);
									row.center = (float)centerDist(ctx->glitch_rng);
									ctx->glitch_sort_rows.push_back(row);
								}
							}

							ctx->glitch_tears.clear();
							if (chanceDist(ctx->glitch_rng) < (s.glitch_tear_chance / 100.0)) {
								std::uniform_int_distribution<int> countDist(1, std::max(1, s.glitch_tear_max_count));
								int tearCount = countDist(ctx->glitch_rng);
								std::uniform_int_distribution<int> hDist(1, std::max(1, s.glitch_tear_max_height));
								std::uniform_int_distribution<int> yDist(0, std::max(0, s.card_h - 1));
								int maxOffset = std::max(0, s.glitch_tear_max_offset);
								std::uniform_int_distribution<int> offsetDist(-maxOffset, maxOffset);
								for (int i = 0; i < tearCount; i++) {
									GlitchTear tear;
									tear.h = hDist(ctx->glitch_rng);
									tear.y = std::clamp(yDist(ctx->glitch_rng), 0, std::max(0, s.card_h - tear.h));
									tear.offsetX = offsetDist(ctx->glitch_rng);
									bool duplicate = chanceDist(ctx->glitch_rng) < (s.glitch_tear_duplicate_chance / 100.0);
									tear.srcY = duplicate ? std::clamp(yDist(ctx->glitch_rng), 0, std::max(0, s.card_h - tear.h)) : tear.y;
									ctx->glitch_tears.push_back(tear);
								}
							}

							ctx->glitch_channel_blocks.clear();
							if (chanceDist(ctx->glitch_rng) < (s.glitch_channel_block_chance / 100.0)) {
								std::uniform_int_distribution<int> countDist(1, std::max(1, s.glitch_channel_block_max_count));
								int blockCount = countDist(ctx->glitch_rng);
								std::uniform_int_distribution<int> sizeDist(2, std::max(2, s.glitch_channel_block_max_size));
								std::uniform_int_distribution<int> xDist(0, std::max(0, s.card_w - 1));
								std::uniform_int_distribution<int> yDist(0, std::max(0, s.card_h - 1));
								std::uniform_int_distribution<int> channelDist(0, 2);
								int maxOffset = std::max(0, s.glitch_channel_block_max_offset);
								std::uniform_int_distribution<int> offsetDist(-maxOffset, maxOffset);
								for (int i = 0; i < blockCount; i++) {
									GlitchChannelBlock block;
									block.w = sizeDist(ctx->glitch_rng);
									block.h = sizeDist(ctx->glitch_rng);
									block.x = std::clamp(xDist(ctx->glitch_rng), 0, std::max(0, s.card_w - block.w));
									block.y = std::clamp(yDist(ctx->glitch_rng), 0, std::max(0, s.card_h - block.h));
									block.channel = channelDist(ctx->glitch_rng);
									block.offsetX = offsetDist(ctx->glitch_rng);
									block.offsetY = offsetDist(ctx->glitch_rng);
									ctx->glitch_channel_blocks.push_back(block);
								}
							}

							needCompose = true;
						}

						if (needCompose) {
							compose_bitmap(ctx, ctx->last_song, ctx->last_artist, s);
						}
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		} catch (const winrt::hresult_error &ex) {
			blog(LOG_ERROR, "[spotify_now_playing] poll_loop iteration failed: %ls", ex.message().c_str());
			std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
		} catch (const std::exception &ex) {
			blog(LOG_ERROR, "[spotify_now_playing] poll_loop iteration failed: %s", ex.what());
			std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
		} catch (...) {
			blog(LOG_ERROR, "[spotify_now_playing] poll_loop iteration failed: unknown exception");
			std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
		}
	}

	// Release explicitly, on this same thread
	sessionManager = nullptr;

	if (com_initialized)
		winrt::uninit_apartment();
}

void InitSourcesLists(spotify_source *ctx)
{
	std::string output;

	std::vector<const char *> musicSources;
	std::vector<const char *> browserSources;

	try {
		ctx->musicSystemStrings = LoadStringList("media-sources-music.txt");
		ctx->browserMediaSourceStrings = LoadStringList("media-sources-browser.txt");

		if (ctx->musicSystemStrings.empty()) {
			musicSources.assign(std::begin(DEFAULT_MUSIC_SYSTEMS), std::end(DEFAULT_MUSIC_SYSTEMS));
			blog(LOG_WARNING, "[spotify_now_playing] possiblemusicsystems came back empty, reverting to defaults");
		} else {
			musicSources.reserve(ctx->musicSystemStrings.size());
			for (const auto &s : ctx->musicSystemStrings) {
				musicSources.push_back(s.c_str());
			}
		}

		if (ctx->browserMediaSourceStrings.empty()) {
			browserSources.assign(std::begin(DEFAULT_BROWSER_SOURCES), std::end(DEFAULT_BROWSER_SOURCES));
			blog(LOG_WARNING, "[spotify_now_playing] possiblebrowsers came back empty, reverting to defaults");
		} else {
			browserSources.reserve(ctx->browserMediaSourceStrings.size());
			for (const auto &s : ctx->browserMediaSourceStrings) {
				browserSources.push_back(s.c_str());
			}
		}
	} catch (...) {
		ctx->musicSystemStrings.clear();
		ctx->browserMediaSourceStrings.clear();
		musicSources.clear();
		browserSources.clear();

		musicSources.assign(std::begin(DEFAULT_MUSIC_SYSTEMS), std::end(DEFAULT_MUSIC_SYSTEMS));
		browserSources.assign(std::begin(DEFAULT_BROWSER_SOURCES), std::end(DEFAULT_BROWSER_SOURCES));

		blog(LOG_WARNING, "[spotify_now_playing] Unknown error reading in music or browser systems, reverting to defaults");
	}

	ctx->kPossibleMusicSystems.clear();
	ctx->kPossibleMusicSystems.reserve(musicSources.size());
	for (const char *system : musicSources) {
		ctx->kPossibleMusicSystems.push_back(ToLowerWide(Utf8ToWide(system)));

		if (!output.empty())
			output += ", ";

		output += "\"";
		output += system;
		output += "\"";
	}

	blog(LOG_INFO, "[spotify_now_playing] Using this list of music sources: %s", output.c_str());

	output.clear();

	ctx->kPossibleBrowserMediaSources.clear();
	ctx->kPossibleBrowserMediaSources.reserve(browserSources.size());
	for (const char *system : browserSources) {
		ctx->kPossibleBrowserMediaSources.push_back(ToLowerWide(Utf8ToWide(system)));

		if (!output.empty())
			output += ", ";

		output += "\"";
		output += system;
		output += "\"";
	}

	blog(LOG_INFO, "[spotify_now_playing] Using this list of browser sources: %s", output.c_str());
}

// ---------------------------------------------------------------------
// obs_source_info callbacks
// ---------------------------------------------------------------------

static const char *spotify_source_get_name(void *)
{
	return obs_module_text("NowPlayingWidget");
}

// ---------------------------------------------------------------------
// Settings export / import
//
// These lists are the single source of truth for which settings keys get
// backed up. Add a key here whenever a new setting is introduced  On import,
// // a key that isn't present in the file being imported is left untouched
// ---------------------------------------------------------------------

static const char *const kSettingsIntKeys[] = {
	"title_color", "artist_color", "bg_color", "bg_opacity", "background_corner_radius", "album_art_corner_radius", "card_width", "card_height", "text_offset_y", "progress_bar_gap", "progress_bar_height", "scroll_speed_ms", "vu_color", "vu_update_ms", "vu_randomness", "vu_width", "vu_height", "vu_bar_count", "progress_fill_color", "progress_bg_color", "autohide_after_s", "title_outline_size", "title_outline_color", "artist_outline_size", "artist_outline_color", "album_art_bg_blur", "vhs_intensity", "vhs_chroma_aberration", "vhs_smear_amount", "vhs_scanline_spacing", "vhs_scanline_intensity", "vhs_tracking_min_interval_s", "vhs_tracking_max_interval_s", "vhs_tracking_line_min_count", "vhs_tracking_line_max_count", "vhs_tracking_line_gap", "vhs_tracking_min_thickness", "vhs_tracking_max_thickness", "vhs_glitch_chance_pct", "vhs_glitch_max_bands", "vhs_grain_amount", "eightmm_intensity", "eightmm_light_leak_intensity", "eightmm_scratch_intensity", "eightmm_dust_intensity", "eightmm_scratch_max_count", "eightmm_dust_max_count", "duotone_shadow_color", "duotone_highlight_color", "duotone_intensity", "bw_desaturation", "bw_contrast", "glitch_intensity", "glitch_pixel_sort_chance", "glitch_pixel_sort_max_rows", "glitch_pixel_sort_threshold", "glitch_tear_chance", "glitch_tear_max_count", "glitch_tear_max_height", "glitch_tear_max_offset", "glitch_tear_duplicate_chance", "glitch_channel_block_chance", "glitch_channel_block_max_count", "glitch_channel_block_max_size", "glitch_channel_block_max_offset",
};

static const char *const kSettingsBoolKeys[] = {
	"use_bg_image", "vu_meter_enabled", "vu_horizontal", "vertical_layout", "show_album_name", "show_goat_placeholder", "show_plugin_attribution", "hide_album_art", "show_progress_bar", "track_change_animation_enabled", "autohide_enabled", "autohide_when_not_playing", "title_outline_enabled", "artist_outline_enabled", "use_album_art_as_bg",
};

static const char *const kSettingsStringKeys[] = {
	"bg_image_path",
	"card_style",
	"eightmm_light_leak_position",
};

static const char *const kSettingsDoubleKeys[] = {
	"vhs_smear_burst_min_s", "vhs_smear_burst_max_s", "vhs_smear_min_interval_s", "vhs_smear_max_interval_s", "vhs_tracking_jitter_min", "vhs_tracking_jitter_max", "vhs_tracking_brighten", "eightmm_vignette_strength", "eightmm_warmth", "eightmm_light_leak_alpha", "eightmm_weave_px", "eightmm_flicker", "bw_vignette_strength",
};

static const char *const kSettingsObjKeys[] = {
	"title_font",
	"artist_font",
};

static void export_known_settings(obs_data_t *settings, obs_data_t *out)
{
	for (const char *key : kSettingsIntKeys)
		obs_data_set_int(out, key, obs_data_get_int(settings, key));
	for (const char *key : kSettingsBoolKeys)
		obs_data_set_bool(out, key, obs_data_get_bool(settings, key));
	for (const char *key : kSettingsStringKeys)
		obs_data_set_string(out, key, obs_data_get_string(settings, key));
	for (const char *key : kSettingsDoubleKeys)
		obs_data_set_double(out, key, obs_data_get_double(settings, key));
	for (const char *key : kSettingsObjKeys) {
		obs_data_t *obj = obs_data_get_obj(settings, key);
		if (obj) {
			obs_data_set_obj(out, key, obj);
			obs_data_release(obj);
		}
	}
}

static void import_known_settings(obs_data_t *imported, obs_data_t *settings)
{
	for (const char *key : kSettingsIntKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_int(settings, key, obs_data_get_int(imported, key));
	for (const char *key : kSettingsBoolKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_bool(settings, key, obs_data_get_bool(imported, key));
	for (const char *key : kSettingsStringKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_string(settings, key, obs_data_get_string(imported, key));
	for (const char *key : kSettingsDoubleKeys)
		if (obs_data_has_user_value(imported, key))
			obs_data_set_double(settings, key, obs_data_get_double(imported, key));
	for (const char *key : kSettingsObjKeys) {
		if (obs_data_has_user_value(imported, key)) {
			obs_data_t *obj = obs_data_get_obj(imported, key);
			if (obj) {
				obs_data_set_obj(settings, key, obj);
				obs_data_release(obj);
			}
		}
	}
}

static bool export_settings_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "export_settings_path");
	if (path && path[0]) {
		struct dstr fixed_path = {0};
		dstr_copy(&fixed_path, path);

		const char *ext = os_get_path_extension(fixed_path.array);
		if (!ext || astrcmpi(ext, ".json") != 0) {
			if (ext && *ext) {
				dstr_resize(&fixed_path, ext - fixed_path.array);
			}
			dstr_cat(&fixed_path, ".json");
		}

		obs_data_t *out = obs_data_create();
		export_known_settings(settings, out);
		if (!obs_data_save_json_pretty_safe(out, fixed_path.array, "tmp", "bak")) {
			blog(LOG_ERROR, "[spotify_now_playing] Failed to export settings to %s", fixed_path.array);
		}
		obs_data_release(out);

		dstr_free(&fixed_path);

		obs_data_set_string(settings, "export_settings_path", "");
	}
	return true;
}

static bool import_settings_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "import_settings_path");
	if (path && path[0]) {
		obs_data_t *imported = obs_data_create_from_json_file(path);
		if (imported) {
			import_known_settings(imported, settings);
			obs_data_release(imported);
		} else {
			blog(LOG_ERROR, "[spotify_now_playing] Failed to import settings from %s", path);
		}

		obs_data_set_string(settings, "import_settings_path", "");
	}
	return true;
}

static void apply_settings(spotify_source *ctx, obs_data_t *settings)
{
	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	ctx->title_color = obs_data_get_int(settings, "title_color");
	ctx->artist_color = obs_data_get_int(settings, "artist_color");

	ctx->title_outline_enabled = obs_data_get_bool(settings, "title_outline_enabled");
	ctx->title_outline_size = (int)obs_data_get_int(settings, "title_outline_size");
	ctx->title_outline_size = std::clamp(ctx->title_outline_size, 1, 50);
	ctx->title_outline_color = obs_data_get_int(settings, "title_outline_color");

	ctx->artist_outline_enabled = obs_data_get_bool(settings, "artist_outline_enabled");
	ctx->artist_outline_size = (int)obs_data_get_int(settings, "artist_outline_size");
	ctx->artist_outline_size = std::clamp(ctx->artist_outline_size, 1, 50);
	ctx->artist_outline_color = obs_data_get_int(settings, "artist_outline_color");

	const char *card_style = obs_data_get_string(settings, "card_style");
	ctx->card_style = card_style ? card_style : "none";

	ctx->vhs_intensity = (int)obs_data_get_int(settings, "vhs_intensity");
	ctx->vhs_intensity = std::clamp(ctx->vhs_intensity, 0, 100);

	ctx->vhs_chroma_aberration = (int)obs_data_get_int(settings, "vhs_chroma_aberration");
	ctx->vhs_chroma_aberration = std::clamp(ctx->vhs_chroma_aberration, 0, 100);

	ctx->vhs_smear_amount = (int)obs_data_get_int(settings, "vhs_smear_amount");
	ctx->vhs_smear_amount = std::clamp(ctx->vhs_smear_amount, 0, 100);

	ctx->vhs_smear_burst_min_s = std::max(1.0, obs_data_get_double(settings, "vhs_smear_burst_min_s"));
	ctx->vhs_smear_burst_max_s = std::max(ctx->vhs_smear_burst_min_s, obs_data_get_double(settings, "vhs_smear_burst_min_s"));

	ctx->vhs_smear_min_interval_s = std::max(1.0, obs_data_get_double(settings, "vhs_smear_min_interval_s"));
	ctx->vhs_smear_max_interval_s = std::max(ctx->vhs_smear_max_interval_s, obs_data_get_double(settings, "vhs_smear_min_interval_s"));

	ctx->vhs_scanline_spacing = std::max(1, (int)obs_data_get_int(settings, "vhs_scanline_spacing"));
	ctx->vhs_scanline_intensity = std::clamp((int)obs_data_get_int(settings, "vhs_scanline_intensity"), 0, 100);
	ctx->vhs_tracking_min_interval_s = std::max(1, (int)obs_data_get_int(settings, "vhs_tracking_min_interval_s"));
	ctx->vhs_tracking_max_interval_s = std::max(ctx->vhs_tracking_min_interval_s, (int)obs_data_get_int(settings, "vhs_tracking_max_interval_s"));
	ctx->vhs_tracking_line_min_count = std::clamp((int)obs_data_get_int(settings, "vhs_tracking_line_min_count"), 1, VHS_TRACKING_LINE_ARRAY_CAP);
	ctx->vhs_tracking_line_max_count = std::clamp((int)obs_data_get_int(settings, "vhs_tracking_line_max_count"), ctx->vhs_tracking_line_min_count, VHS_TRACKING_LINE_ARRAY_CAP);
	ctx->vhs_tracking_line_gap = std::max(0, (int)obs_data_get_int(settings, "vhs_tracking_line_gap"));
	ctx->vhs_tracking_min_thickness = std::max(1, (int)obs_data_get_int(settings, "vhs_tracking_min_thickness"));
	ctx->vhs_tracking_max_thickness = std::max(ctx->vhs_tracking_min_thickness, (int)obs_data_get_int(settings, "vhs_tracking_max_thickness"));
	ctx->vhs_tracking_jitter_min = obs_data_get_double(settings, "vhs_tracking_jitter_min");
	ctx->vhs_tracking_jitter_max = obs_data_get_double(settings, "vhs_tracking_jitter_max");
	ctx->vhs_tracking_brighten = std::clamp(obs_data_get_double(settings, "vhs_tracking_brighten"), 0.0, 1.0);
	ctx->vhs_glitch_chance_pct = std::clamp((int)obs_data_get_int(settings, "vhs_glitch_chance_pct"), 0, 100);
	ctx->vhs_glitch_max_bands = std::max(1, (int)obs_data_get_int(settings, "vhs_glitch_max_bands"));
	ctx->vhs_grain_amount = std::clamp((int)obs_data_get_int(settings, "vhs_grain_amount"), 0, 100);

	ctx->eightmm_intensity = (int)obs_data_get_int(settings, "eightmm_intensity");
	ctx->eightmm_intensity = std::clamp(ctx->eightmm_intensity, 0, 100);

	ctx->eightmm_vignette_strength = std::clamp(obs_data_get_double(settings, "eightmm_vignette_strength"), 0.0, 1.0);
	ctx->eightmm_warmth = obs_data_get_double(settings, "eightmm_warmth");
	ctx->eightmm_light_leak_alpha = std::clamp(obs_data_get_double(settings, "eightmm_light_leak_alpha"), 0.0, 1.0);
	const char *eightmm_light_leak_position = obs_data_get_string(settings, "eightmm_light_leak_position");
	ctx->eightmm_light_leak_position = (eightmm_light_leak_position && eightmm_light_leak_position[0]) ? eightmm_light_leak_position : "none";
	ctx->eightmm_light_leak_intensity = std::clamp((int)obs_data_get_int(settings, "eightmm_light_leak_intensity"), 1, 100);
	ctx->eightmm_weave_px = std::max(0.0, obs_data_get_double(settings, "eightmm_weave_px"));
	ctx->eightmm_flicker = std::clamp(obs_data_get_double(settings, "eightmm_flicker"), 0.0, 1.0);
	ctx->eightmm_scratch_intensity = std::clamp((int)obs_data_get_int(settings, "eightmm_scratch_intensity"), 0, 100);
	ctx->eightmm_dust_intensity = std::clamp((int)obs_data_get_int(settings, "eightmm_dust_intensity"), 0, 100);
	ctx->eightmm_scratch_max_count = std::max(0, (int)obs_data_get_int(settings, "eightmm_scratch_max_count"));
	ctx->eightmm_dust_max_count = std::max(0, (int)obs_data_get_int(settings, "eightmm_dust_max_count"));

	ctx->duotone_shadow_color = obs_data_get_int(settings, "duotone_shadow_color");
	ctx->duotone_highlight_color = obs_data_get_int(settings, "duotone_highlight_color");
	ctx->duotone_intensity = (int)obs_data_get_int(settings, "duotone_intensity");
	ctx->duotone_intensity = std::clamp(ctx->duotone_intensity, 0, 100);

	ctx->bw_desaturation = std::clamp((int)obs_data_get_int(settings, "bw_desaturation"), 0, 100);
	ctx->bw_contrast = std::clamp((int)obs_data_get_int(settings, "bw_contrast"), -100, 100);
	ctx->bw_vignette_strength = std::clamp(obs_data_get_double(settings, "bw_vignette_strength"), 0.0, 1.0);

	ctx->glitch_intensity = std::clamp((int)obs_data_get_int(settings, "glitch_intensity"), 0, 100);
	ctx->glitch_pixel_sort_chance = std::clamp((int)obs_data_get_int(settings, "glitch_pixel_sort_chance"), 0, 100);
	ctx->glitch_pixel_sort_max_rows = std::max(1, (int)obs_data_get_int(settings, "glitch_pixel_sort_max_rows"));
	ctx->glitch_pixel_sort_threshold = std::clamp((int)obs_data_get_int(settings, "glitch_pixel_sort_threshold"), 0, 100);
	ctx->glitch_tear_chance = std::clamp((int)obs_data_get_int(settings, "glitch_tear_chance"), 0, 100);
	ctx->glitch_tear_max_count = std::max(0, (int)obs_data_get_int(settings, "glitch_tear_max_count"));
	ctx->glitch_tear_max_height = std::max(1, (int)obs_data_get_int(settings, "glitch_tear_max_height"));
	ctx->glitch_tear_max_offset = std::max(0, (int)obs_data_get_int(settings, "glitch_tear_max_offset"));
	ctx->glitch_tear_duplicate_chance = std::clamp((int)obs_data_get_int(settings, "glitch_tear_duplicate_chance"), 0, 100);
	ctx->glitch_channel_block_chance = std::clamp((int)obs_data_get_int(settings, "glitch_channel_block_chance"), 0, 100);
	ctx->glitch_channel_block_max_count = std::max(0, (int)obs_data_get_int(settings, "glitch_channel_block_max_count"));
	ctx->glitch_channel_block_max_size = std::max(1, (int)obs_data_get_int(settings, "glitch_channel_block_max_size"));
	ctx->glitch_channel_block_max_offset = std::max(0, (int)obs_data_get_int(settings, "glitch_channel_block_max_offset"));

	ctx->bg_color = obs_data_get_int(settings, "bg_color");

	ctx->bg_opacity = (int)obs_data_get_int(settings, "bg_opacity");
	ctx->bg_opacity = std::clamp(ctx->bg_opacity, 0, 100);

	ctx->use_bg_image = obs_data_get_bool(settings, "use_bg_image");
	const char *bg_image_path = obs_data_get_string(settings, "bg_image_path");
	ctx->bg_image_path = bg_image_path ? bg_image_path : "";

	ctx->use_album_art_as_bg = obs_data_get_bool(settings, "use_album_art_as_bg");
	ctx->album_art_bg_blur_pct = (int)obs_data_get_int(settings, "album_art_bg_blur");
	ctx->album_art_bg_blur_pct = std::clamp(ctx->album_art_bg_blur_pct, 0, 100);

	ctx->background_corner_radius = (int)obs_data_get_int(settings, "background_corner_radius");
	ctx->background_corner_radius = std::clamp(ctx->background_corner_radius, 0, 100);

	ctx->album_art_corner_radius = (int)obs_data_get_int(settings, "album_art_corner_radius");
	ctx->album_art_corner_radius = std::clamp(ctx->album_art_corner_radius, 0, 100);

	ctx->card_w = (int)obs_data_get_int(settings, "card_width");
	ctx->card_w = std::clamp(ctx->card_w, 50, 4000);

	ctx->card_h = (int)obs_data_get_int(settings, "card_height");
	ctx->card_h = std::clamp(ctx->card_h, 30, 2000);

	ctx->text_offset_y = (int)obs_data_get_int(settings, "text_offset_y");
	ctx->text_offset_y = std::clamp(ctx->text_offset_y, -1000, 1000);

	ctx->progress_bar_gap = (int)obs_data_get_int(settings, "progress_bar_gap");
	ctx->progress_bar_gap = std::clamp(ctx->progress_bar_gap, -1000, 1000);

	ctx->progress_bar_height = (int)obs_data_get_int(settings, "progress_bar_height");
	ctx->progress_bar_height = std::clamp(ctx->progress_bar_height, 2, 1000);

	ctx->scroll_speed_ms = (int)obs_data_get_int(settings, "scroll_speed_ms");
	ctx->scroll_speed_ms = std::clamp(ctx->scroll_speed_ms, 20, 5000);

	ctx->browser_media_source_enabled = obs_data_get_bool(settings, "enable_browser_media");

	ctx->vu_meter_enabled = obs_data_get_bool(settings, "vu_meter_enabled");
	ctx->vu_color = obs_data_get_int(settings, "vu_color");

	ctx->vu_update_ms = (int)obs_data_get_int(settings, "vu_update_ms");
	ctx->vu_update_ms = std::clamp(ctx->vu_update_ms, 50, 2000);

	ctx->vu_randomness = (int)obs_data_get_int(settings, "vu_randomness");
	ctx->vu_randomness = std::clamp(ctx->vu_randomness, 0, 100);

	ctx->vu_width = (int)obs_data_get_int(settings, "vu_width");
	ctx->vu_width = std::clamp(ctx->vu_width, 4, 2000);

	ctx->vu_height = (int)obs_data_get_int(settings, "vu_height");
	ctx->vu_height = std::clamp(ctx->vu_height, 4, 2000);

	ctx->vu_bar_count = (int)obs_data_get_int(settings, "vu_bar_count");
	ctx->vu_bar_count = std::clamp(ctx->vu_bar_count, 1, VU_MAX_BAR_COUNT);

	ctx->vu_horizontal = obs_data_get_bool(settings, "vu_horizontal");
	ctx->vertical_layout = obs_data_get_bool(settings, "vertical_layout");
	ctx->show_album_name = obs_data_get_bool(settings, "show_album_name");
	ctx->show_goat_placeholder = obs_data_get_bool(settings, "show_goat_placeholder");
	ctx->show_plugin_attribution = obs_data_get_bool(settings, "show_plugin_attribution");
	ctx->hide_album_art = obs_data_get_bool(settings, "hide_album_art");

	ctx->show_progress_bar = obs_data_get_bool(settings, "show_progress_bar");
	ctx->progress_fill_color = obs_data_get_int(settings, "progress_fill_color");
	ctx->progress_bg_color = obs_data_get_int(settings, "progress_bg_color");

	ctx->track_change_animation_enabled = obs_data_get_bool(settings, "track_change_animation_enabled");

	ctx->autohide_enabled = obs_data_get_bool(settings, "autohide_enabled");
	ctx->autohide_after_s = (int)obs_data_get_int(settings, "autohide_after_s");
	ctx->autohide_after_s = std::clamp(ctx->autohide_after_s, 0, 3600);

	ctx->autohide_when_not_playing = obs_data_get_bool(settings, "autohide_when_not_playing");

	obs_data_t *title_font_obj = obs_data_get_obj(settings, "title_font");
	if (title_font_obj) {
		const char *face = obs_data_get_string(title_font_obj, "face");
		const char *style = obs_data_get_string(title_font_obj, "style");
		ctx->title_font_face = (face && face[0]) ? face : "Segoe UI";
		ctx->title_font_style = style ? style : "Regular";
		ctx->title_font_size = (int)obs_data_get_int(title_font_obj, "size");
		ctx->title_font_flags = (int)obs_data_get_int(title_font_obj, "flags");
		obs_data_release(title_font_obj);
	}
	if (ctx->title_font_size <= 0) {
		ctx->title_font_size = DEFAULT_TITLE_FONT_SIZE;
	}

	obs_data_t *artist_font_obj = obs_data_get_obj(settings, "artist_font");
	if (artist_font_obj) {
		const char *face = obs_data_get_string(artist_font_obj, "face");
		const char *style = obs_data_get_string(artist_font_obj, "style");
		ctx->artist_font_face = (face && face[0]) ? face : "Segoe UI";
		ctx->artist_font_style = style ? style : "Regular";
		ctx->artist_font_size = (int)obs_data_get_int(artist_font_obj, "size");
		ctx->artist_font_flags = (int)obs_data_get_int(artist_font_obj, "flags");
		obs_data_release(artist_font_obj);
	}
	if (ctx->artist_font_size <= 0) {
		ctx->artist_font_size = DEFAULT_ARTIST_FONT_SIZE;
	}

	ctx->settings_dirty = true;
}

static void spotify_source_update(void *data, obs_data_t *settings)
{
	auto *ctx = (spotify_source *)data;
	try {
		apply_settings(ctx, settings);
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_update failed: %s", ex.what());
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_update failed: Unknown exception");
	}
}

static void spotify_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "enable_browser_media", DEFAULT_ENABLE_BROWSER_MEDIA_SOURCES);
	obs_data_set_default_int(settings, "title_color", DEFAULT_COLOR_WHITE);
	obs_data_set_default_int(settings, "artist_color", DEFAULT_COLOR_WHITE);

	obs_data_set_default_bool(settings, "title_outline_enabled", false);
	obs_data_set_default_int(settings, "title_outline_size", DEFAULT_TEXT_OUTLINE_SIZE_PX);
	obs_data_set_default_int(settings, "title_outline_color", DEFAULT_COLOR_BLACK);

	obs_data_set_default_bool(settings, "artist_outline_enabled", false);
	obs_data_set_default_int(settings, "artist_outline_size", DEFAULT_TEXT_OUTLINE_SIZE_PX);
	obs_data_set_default_int(settings, "artist_outline_color", DEFAULT_COLOR_BLACK);

	obs_data_set_default_string(settings, "card_style", "none");
	obs_data_set_default_int(settings, "vhs_intensity", DEFAULT_VHS_INTENSITY);
	obs_data_set_default_int(settings, "vhs_chroma_aberration", DEFAULT_VHS_CHROMA_ABERRATION);
	obs_data_set_default_int(settings, "vhs_smear_amount", DEFAULT_VHS_SMEAR_AMOUNT);
	obs_data_set_default_double(settings, "vhs_smear_burst_min_s", DEFAULT_VHS_SMEAR_BURST_MIN_S);
	obs_data_set_default_double(settings, "vhs_smear_burst_max_s", DEFAULT_VHS_SMEAR_BURST_MAX_S);
	obs_data_set_default_double(settings, "vhs_smear_min_interval_s", DEFAULT_VHS_SMEAR_MIN_INTERVAL_S);
	obs_data_set_default_double(settings, "vhs_smear_max_interval_s", DEFAULT_VHS_SMEAR_MAX_INTERVAL_S);
	obs_data_set_default_int(settings, "vhs_scanline_spacing", DEFAULT_VHS_SCANLINE_SPACING_PX);
	obs_data_set_default_int(settings, "vhs_scanline_intensity", DEFAULT_VHS_SCANLINE_INTENSITY);
	obs_data_set_default_int(settings, "vhs_tracking_min_interval_s", DEFAULT_VHS_TRACKING_MIN_INTERVAL_S);
	obs_data_set_default_int(settings, "vhs_tracking_max_interval_s", DEFAULT_VHS_TRACKING_MAX_INTERVAL_S);
	obs_data_set_default_int(settings, "vhs_tracking_line_min_count", DEFAULT_VHS_TRACKING_LINE_MIN_COUNT);
	obs_data_set_default_int(settings, "vhs_tracking_line_max_count", DEFAULT_VHS_TRACKING_LINE_MAX_COUNT);
	obs_data_set_default_int(settings, "vhs_tracking_line_gap", DEFAULT_VHS_TRACKING_LINE_GAP_PX);
	obs_data_set_default_int(settings, "vhs_tracking_min_thickness", DEFAULT_VHS_TRACKING_MIN_THICKNESS_PX);
	obs_data_set_default_int(settings, "vhs_tracking_max_thickness", DEFAULT_VHS_TRACKING_MAX_THICKNESS_PX);
	obs_data_set_default_double(settings, "vhs_tracking_jitter_min", DEFAULT_VHS_TRACKING_JITTER_MIN_PX);
	obs_data_set_default_double(settings, "vhs_tracking_jitter_max", DEFAULT_VHS_TRACKING_JITTER_MAX_PX);
	obs_data_set_default_double(settings, "vhs_tracking_brighten", DEFAULT_VHS_TRACKING_BRIGHTEN);
	obs_data_set_default_int(settings, "vhs_glitch_chance_pct", DEFAULT_VHS_GLITCH_CHANCE_PCT);
	obs_data_set_default_int(settings, "vhs_glitch_max_bands", DEFAULT_VHS_GLITCH_MAX_BANDS);
	obs_data_set_default_int(settings, "vhs_grain_amount", DEFAULT_VHS_GRAIN_AMOUNT);
	obs_data_set_default_int(settings, "eightmm_intensity", DEFAULT_EIGHTMM_INTENSITY);
	obs_data_set_default_double(settings, "eightmm_vignette_strength", DEFAULT_EIGHTMM_VIGNETTE_STRENGTH);
	obs_data_set_default_double(settings, "eightmm_warmth", DEFAULT_EIGHTMM_WARMTH);
	obs_data_set_default_double(settings, "eightmm_light_leak_alpha", DEFAULT_EIGHTMM_LIGHT_LEAK_ALPHA);
	obs_data_set_default_string(settings, "eightmm_light_leak_position", DEFAULT_EIGHTMM_LIGHT_LEAK_POSITION);
	obs_data_set_default_int(settings, "eightmm_light_leak_intensity", DEFAULT_EIGHTMM_LIGHT_LEAK_INTENSITY);
	obs_data_set_default_double(settings, "eightmm_weave_px", DEFAULT_EIGHTMM_WEAVE_PX);
	obs_data_set_default_double(settings, "eightmm_flicker", DEFAULT_EIGHTMM_FLICKER);
	obs_data_set_default_int(settings, "eightmm_scratch_intensity", DEFAULT_EIGHTMM_SCRATCH_INTENSITY);
	obs_data_set_default_int(settings, "eightmm_dust_intensity", DEFAULT_EIGHTMM_DUST_INTENSITY);
	obs_data_set_default_int(settings, "eightmm_scratch_max_count", DEFAULT_EIGHTMM_SCRATCH_MAX_COUNT);
	obs_data_set_default_int(settings, "eightmm_dust_max_count", DEFAULT_EIGHTMM_DUST_MAX_COUNT);
	obs_data_set_default_int(settings, "duotone_shadow_color", DEFAULT_DUOTONE_SHADOW_COLOR);
	obs_data_set_default_int(settings, "duotone_highlight_color", DEFAULT_DUOTONE_HIGHLIGHT_COLOR);
	obs_data_set_default_int(settings, "duotone_intensity", DEFAULT_DUOTONE_INTENSITY);
	obs_data_set_default_int(settings, "bw_desaturation", DEFAULT_BW_DESATURATION);
	obs_data_set_default_int(settings, "bw_contrast", DEFAULT_BW_CONTRAST);
	obs_data_set_default_double(settings, "bw_vignette_strength", DEFAULT_BW_VIGNETTE_STRENGTH);

	obs_data_set_default_int(settings, "glitch_intensity", DEFAULT_GLITCH_INTENSITY);
	obs_data_set_default_int(settings, "glitch_pixel_sort_chance", DEFAULT_GLITCH_PIXEL_SORT_CHANCE);
	obs_data_set_default_int(settings, "glitch_pixel_sort_max_rows", DEFAULT_GLITCH_PIXEL_SORT_MAX_ROWS);
	obs_data_set_default_int(settings, "glitch_pixel_sort_threshold", DEFAULT_GLITCH_PIXEL_SORT_THRESHOLD);
	obs_data_set_default_int(settings, "glitch_tear_chance", DEFAULT_GLITCH_TEAR_CHANCE);
	obs_data_set_default_int(settings, "glitch_tear_max_count", DEFAULT_GLITCH_TEAR_MAX_COUNT);
	obs_data_set_default_int(settings, "glitch_tear_max_height", DEFAULT_GLITCH_TEAR_MAX_HEIGHT);
	obs_data_set_default_int(settings, "glitch_tear_max_offset", DEFAULT_GLITCH_TEAR_MAX_OFFSET);
	obs_data_set_default_int(settings, "glitch_tear_duplicate_chance", DEFAULT_GLITCH_TEAR_DUPLICATE_CHANCE);
	obs_data_set_default_int(settings, "glitch_channel_block_chance", DEFAULT_GLITCH_CHANNEL_BLOCK_CHANCE);
	obs_data_set_default_int(settings, "glitch_channel_block_max_count", DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_COUNT);
	obs_data_set_default_int(settings, "glitch_channel_block_max_size", DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_SIZE);
	obs_data_set_default_int(settings, "glitch_channel_block_max_offset", DEFAULT_GLITCH_CHANNEL_BLOCK_MAX_OFFSET);

	obs_data_set_default_int(settings, "bg_color", DEFAULT_COLOR_BLACK);
	obs_data_set_default_int(settings, "bg_opacity", DEFAULT_BG_OPACITY);
	obs_data_set_default_string(settings, "export_settings_path", "");
	obs_data_set_default_string(settings, "import_settings_path", "");

	obs_data_set_default_bool(settings, "use_bg_image", false);
	obs_data_set_default_string(settings, "bg_image_path", "");
	obs_data_set_default_bool(settings, "use_album_art_as_bg", false);
	obs_data_set_default_int(settings, "album_art_bg_blur", DEFAULT_ALBUM_ART_BG_BLUR_PCT);
	obs_data_set_default_int(settings, "background_corner_radius", DEFAULT_BACKGROUND_CORNER_RADIUS);
	obs_data_set_default_int(settings, "album_art_corner_radius", DEFAULT_ALBUM_ART_CORNER_RADIUS);

	obs_data_set_default_int(settings, "card_width", DEFAULT_CARD_W);
	obs_data_set_default_int(settings, "card_height", DEFAULT_CARD_H);
	obs_data_set_default_int(settings, "text_offset_y", 0);
	obs_data_set_default_int(settings, "progress_bar_gap", DEFAULT_PROGRESS_BAR_GAP);
	obs_data_set_default_int(settings, "progress_bar_height", DEFAULT_PROGRESS_BAR_HEIGHT);

	obs_data_set_default_int(settings, "scroll_speed_ms", DEFAULT_SCROLL_SPEED_MS);

	obs_data_set_default_bool(settings, "vu_meter_enabled", true);
	obs_data_set_default_int(settings, "vu_color", DEFAULT_COLOR_GREEN);
	obs_data_set_default_int(settings, "vu_update_ms", DEFAULT_ANIMATION_UPDATE_MS);
	obs_data_set_default_int(settings, "vu_randomness", DEFAULT_VU_RANDOMNESS);

	obs_data_set_default_int(settings, "vu_width", DEFAULT_VU_WIDTH);
	obs_data_set_default_int(settings, "vu_height", DEFAULT_VU_HEIGHT);
	obs_data_set_default_int(settings, "vu_bar_count", DEFAULT_VU_BAR_COUNT);
	obs_data_set_default_bool(settings, "vu_horizontal", false);

	obs_data_set_default_bool(settings, "vertical_layout", false);
	obs_data_set_default_bool(settings, "show_goat_placeholder", true);
	obs_data_set_default_bool(settings, "show_plugin_attribution", true);
	obs_data_set_default_bool(settings, "hide_album_art", false);
	obs_data_set_default_bool(settings, "show_album_name", false);

	obs_data_set_default_bool(settings, "show_progress_bar", true);
	obs_data_set_default_int(settings, "progress_fill_color", DEFAULT_COLOR_WHITE);
	obs_data_set_default_int(settings, "progress_bg_color", DEFAULT_COLOR_DARK_GREY);

	obs_data_set_default_bool(settings, "track_change_animation_enabled", true);

	obs_data_set_default_bool(settings, "autohide_enabled", false);
	obs_data_set_default_int(settings, "autohide_after_s", DEFAULT_AUTOHIDE_AFTER_S);

	obs_data_set_default_bool(settings, "autohide_when_not_playing", false);

	obs_data_t *title_font_obj = obs_data_create();
	obs_data_set_default_string(title_font_obj, "face", "Segoe UI");
	obs_data_set_default_string(title_font_obj, "style", "Bold");
	obs_data_set_default_int(title_font_obj, "size", DEFAULT_TITLE_FONT_SIZE);
	obs_data_set_default_obj(settings, "title_font", title_font_obj);
	obs_data_release(title_font_obj);

	obs_data_t *artist_font_obj = obs_data_create();
	obs_data_set_default_string(artist_font_obj, "face", "Segoe UI");
	obs_data_set_default_string(artist_font_obj, "style", "Regular");
	obs_data_set_default_int(artist_font_obj, "size", DEFAULT_ARTIST_FONT_SIZE);
	obs_data_set_default_obj(settings, "artist_font", artist_font_obj);
	obs_data_release(artist_font_obj);
}

static bool autohide_enabled_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "autohide_enabled");
	obs_property_set_enabled(obs_properties_get(props, "autohide_after_s"), enabled);
	return true;
}

static bool show_vu_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "vu_meter_enabled");
	obs_property_set_enabled(obs_properties_get(props, "vu_horizontal"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "vu_color"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "vu_update_ms"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "vu_randomness"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "vu_width"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "vu_height"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "vu_bar_count"), enabled);
	return true;
}

static bool title_outline_enabled_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "title_outline_enabled");
	obs_property_set_enabled(obs_properties_get(props, "title_outline_size"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "title_outline_color"), enabled);
	return true;
}

static bool artist_outline_enabled_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "artist_outline_enabled");
	obs_property_set_enabled(obs_properties_get(props, "artist_outline_size"), enabled);
	obs_property_set_enabled(obs_properties_get(props, "artist_outline_color"), enabled);
	return true;
}

static bool use_bg_image_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool useArt = obs_data_get_bool(settings, "use_album_art_as_bg");
	bool enabled = obs_data_get_bool(settings, "use_bg_image");
	obs_property_set_enabled(obs_properties_get(props, "bg_color"), !enabled);
	obs_property_set_enabled(obs_properties_get(props, "bg_image_path"), enabled && !useArt);
	obs_property_set_enabled(obs_properties_get(props, "use_bg_image"), !useArt);
	return true;
}

static bool use_album_art_as_bg_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool useArt = obs_data_get_bool(settings, "use_album_art_as_bg");
	bool useImg = obs_data_get_bool(settings, "use_bg_image");
	obs_property_set_enabled(obs_properties_get(props, "album_art_bg_blur"), useArt);
	obs_property_set_enabled(obs_properties_get(props, "use_bg_image"), !useArt);
	obs_property_set_enabled(obs_properties_get(props, "bg_image_path"), !useArt && useImg);
	return true;
}

static bool card_style_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const char *styleRaw = obs_data_get_string(settings, "card_style");
	std::string style = styleRaw ? styleRaw : "none";
	bool isVhs = style == "vhs";
	bool isEightMm = style == "8mm";
	bool isDuotone = style == "duotone";
	bool isBw = style == "bw";
	bool isGlitch = style == "glitch";

	static const char *const kVhsKeys[] = {
		"vhs_intensity", "vhs_chroma_aberration", "vhs_smear_amount", "vhs_smear_burst_min_s", "vhs_smear_burst_max_s", "vhs_smear_min_interval_s", "vhs_smear_max_interval_s", "vhs_scanline_spacing", "vhs_scanline_intensity", "vhs_tracking_min_interval_s", "vhs_tracking_max_interval_s", "vhs_tracking_line_min_count", "vhs_tracking_line_max_count", "vhs_tracking_line_gap", "vhs_tracking_min_thickness", "vhs_tracking_max_thickness", "vhs_tracking_jitter_min", "vhs_tracking_jitter_max", "vhs_tracking_brighten", "vhs_glitch_chance_pct", "vhs_glitch_max_bands", "vhs_grain_amount",
	};
	static const char *const kEightMmKeys[] = {
		"eightmm_intensity", "eightmm_vignette_strength", "eightmm_warmth", "eightmm_light_leak_alpha", "eightmm_light_leak_position", "eightmm_light_leak_intensity", "eightmm_weave_px", "eightmm_flicker", "eightmm_scratch_intensity", "eightmm_dust_intensity", "eightmm_scratch_max_count", "eightmm_dust_max_count",
	};
	static const char *const kDuotoneKeys[] = {
		"duotone_shadow_color",
		"duotone_highlight_color",
		"duotone_intensity",
	};
	static const char *const kBwKeys[] = {
		"bw_desaturation",
		"bw_contrast",
		"bw_vignette_strength",
	};
	static const char *const kGlitchKeys[] = {
		"glitch_intensity", "glitch_pixel_sort_chance", "glitch_pixel_sort_max_rows", "glitch_pixel_sort_threshold", "glitch_tear_chance", "glitch_tear_max_count", "glitch_tear_max_height", "glitch_tear_max_offset", "glitch_tear_duplicate_chance", "glitch_channel_block_chance", "glitch_channel_block_max_count", "glitch_channel_block_max_size", "glitch_channel_block_max_offset",
	};

	for (const char *key : kVhsKeys)
		obs_property_set_visible(obs_properties_get(props, key), isVhs);
	for (const char *key : kEightMmKeys)
		obs_property_set_visible(obs_properties_get(props, key), isEightMm);
	for (const char *key : kDuotoneKeys)
		obs_property_set_visible(obs_properties_get(props, key), isDuotone);
	for (const char *key : kBwKeys)
		obs_property_set_visible(obs_properties_get(props, key), isBw);
	for (const char *key : kGlitchKeys)
		obs_property_set_visible(obs_properties_get(props, key), isGlitch);

	return true;
}

static void spotify_source_properties_impl(obs_properties_t *props, void *data)
{
	obs_properties_add_bool(props, "vertical_layout", obs_module_text("VerticalLayout"));
	obs_properties_add_bool(props, "hide_album_art", obs_module_text("HideAlbumArt"));
	obs_properties_add_bool(props, "show_progress_bar", obs_module_text("ShowProgressBar"));
	obs_properties_add_bool(props, "show_album_name", obs_module_text("ShowAlbumName"));
	obs_properties_add_bool(props, "track_change_animation_enabled", obs_module_text("TrackChangeAnimation"));
	obs_properties_add_bool(props, "autohide_when_not_playing", obs_module_text("AutohideWhenNotPlaying"));
	obs_property_t *autohide_prop = obs_properties_add_bool(props, "autohide_enabled", obs_module_text("AutohideEnabled"));
	obs_properties_add_int(props, "autohide_after_s", obs_module_text("AutohideAfterSeconds"), 1, 3600, 1);
	obs_property_set_modified_callback(autohide_prop, autohide_enabled_modified);
	obs_properties_add_int(props, "card_width", obs_module_text("CardWidth"), 50, 4000, 10);
	obs_properties_add_int(props, "card_height", obs_module_text("CardHeight"), 30, 2000, 10);
	obs_properties_add_font(props, "title_font", obs_module_text("TitleFont"));
	obs_properties_add_font(props, "artist_font", obs_module_text("ArtistFont"));
	obs_properties_add_color_alpha(props, "title_color", obs_module_text("TitleColor"));
	obs_properties_add_color_alpha(props, "artist_color", obs_module_text("ArtistColor"));

	obs_property_t *title_outline_enabled_prop = obs_properties_add_bool(props, "title_outline_enabled", obs_module_text("TitleOutlineEnabled"));
	obs_properties_add_int(props, "title_outline_size", obs_module_text("TitleOutlineSize"), 1, 50, 1);
	obs_properties_add_color_alpha(props, "title_outline_color", obs_module_text("TitleOutlineColor"));
	obs_property_set_modified_callback(title_outline_enabled_prop, title_outline_enabled_modified);

	obs_property_t *artist_outline_enabled_prop = obs_properties_add_bool(props, "artist_outline_enabled", obs_module_text("ArtistOutlineEnabled"));
	obs_properties_add_int(props, "artist_outline_size", obs_module_text("ArtistOutlineSize"), 1, 50, 1);
	obs_properties_add_color_alpha(props, "artist_outline_color", obs_module_text("ArtistOutlineColor"));
	obs_property_set_modified_callback(artist_outline_enabled_prop, artist_outline_enabled_modified);

	obs_property_t *use_album_art_as_bg_prop = obs_properties_add_bool(props, "use_album_art_as_bg", obs_module_text("UseAlbumArtAsBackground"));
	obs_properties_add_int(props, "album_art_bg_blur", obs_module_text("AlbumArtBackgroundBlur"), 0, 100, 1);
	obs_property_set_modified_callback(use_album_art_as_bg_prop, use_album_art_as_bg_modified);
	obs_property_t *use_bg_image_prop = obs_properties_add_bool(props, "use_bg_image", obs_module_text("UseImageAsBackground"));
	obs_properties_add_path(props, "bg_image_path", obs_module_text("BackgroundImagePath"), OBS_PATH_FILE, "Image Files (*.jpg *.jpeg *.png);;All Files (*.*)", nullptr);
	obs_property_set_modified_callback(use_bg_image_prop, use_bg_image_modified);
	obs_properties_add_color(props, "bg_color", obs_module_text("BackgroundColor"));
	obs_properties_add_int(props, "bg_opacity", obs_module_text("BackgroundOpacity"), 0, 100, 1);
	obs_properties_add_int(props, "background_corner_radius", obs_module_text("BackgroundCornerRadius"), 1, 100, 1);
	obs_properties_add_int(props, "album_art_corner_radius", obs_module_text("AlbumArtCornerRadius"), 1, 100, 1);

	obs_properties_add_int(props, "text_offset_y", obs_module_text("TextVerticalOffset"), -1000, 1000, 1);
	obs_properties_add_int(props, "scroll_speed_ms", obs_module_text("ScrollSpeed"), 50, 5000, 10);
	obs_properties_add_color_alpha(props, "progress_fill_color", obs_module_text("ProgressFillColor"));
	obs_properties_add_color_alpha(props, "progress_bg_color", obs_module_text("ProgressBackgroundColor"));
	obs_properties_add_int(props, "progress_bar_height", obs_module_text("ProgressBarHeight"), 2, 1000, 1);
	obs_properties_add_int(props, "progress_bar_gap", obs_module_text("ProgressBarGap"), -1000, 1000, 1);
	obs_property_t *vu_prop = obs_properties_add_bool(props, "vu_meter_enabled", obs_module_text("ShowVUMeter"));
	obs_properties_add_bool(props, "vu_horizontal", obs_module_text("VUMeterHorizontalOrientation"));
	obs_properties_add_color_alpha(props, "vu_color", obs_module_text("VUMeterColor"));
	obs_properties_add_int(props, "vu_update_ms", obs_module_text("VUUpdateSpeed"), 50, 2000, 10);
	obs_properties_add_int(props, "vu_randomness", obs_module_text("VURandomness"), 0, 100, 5);
	obs_properties_add_int(props, "vu_width", obs_module_text("VUMeterWidth"), 4, 2000, 1);
	obs_properties_add_int(props, "vu_height", obs_module_text("VUMeterHeight"), 4, 2000, 1);
	obs_properties_add_int(props, "vu_bar_count", obs_module_text("VUBarCount"), 1, VU_MAX_BAR_COUNT, 1);
	obs_property_set_modified_callback(vu_prop, show_vu_modified);
	obs_properties_add_bool(props, "show_goat_placeholder", obs_module_text("ShowGoatWhenNoAlbumArt"));
	obs_properties_add_bool(props, "show_plugin_attribution", obs_module_text("ShowPluginAttribution"));

	obs_property_t *card_style_prop = obs_properties_add_list(props, "card_style", obs_module_text("CardStyle"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(card_style_prop, obs_module_text("CardStyleNone"), "none");
	obs_property_list_add_string(card_style_prop, obs_module_text("CardStyleVhs"), "vhs");
	obs_property_list_add_string(card_style_prop, obs_module_text("CardStyleEightMm"), "8mm");
	obs_property_list_add_string(card_style_prop, obs_module_text("CardStyleGlitch"), "glitch");
	obs_property_list_add_string(card_style_prop, obs_module_text("CardStyleDuotone"), "duotone");
	obs_property_list_add_string(card_style_prop, obs_module_text("CardStyleBw"), "bw");

	obs_properties_add_int(props, "vhs_intensity", obs_module_text("VhsEffectIntensity"), 0, 100, 1);
	obs_properties_add_int(props, "vhs_chroma_aberration", obs_module_text("VhsChromaticAberration"), 0, 100, 1);
	obs_properties_add_int(props, "vhs_smear_amount", obs_module_text("VhsSmearAmount"), 0, 100, 1);
	obs_properties_add_float(props, "vhs_smear_burst_min_s", obs_module_text("VhsSmearMinLength"), 1.0, 6000.0, 1.0);
	obs_properties_add_float(props, "vhs_smear_burst_max_s", obs_module_text("VhsSmearMaxLength"), 1.0, 6000.0, 1.0);
	obs_properties_add_float(props, "vhs_smear_min_interval_s", obs_module_text("VhsSmearMinInterval"), 1.0, 6000.0, 1.0);
	obs_properties_add_float(props, "vhs_smear_max_interval_s", obs_module_text("VhsSmearMaxInterval"), 1.0, 6000.0, 1.0);

	obs_properties_add_int(props, "vhs_grain_amount", obs_module_text("VhsGrainAmount"), 0, 100, 1);
	obs_properties_add_int(props, "vhs_scanline_spacing", obs_module_text("VhsScanlineSpacing"), 1, 20, 1);
	obs_properties_add_int(props, "vhs_scanline_intensity", obs_module_text("VhsScanlineIntensity"), 0, 100, 1);
	obs_properties_add_int(props, "vhs_tracking_min_interval_s", obs_module_text("VhsTrackingMinInterval"), 1, 60, 1);
	obs_properties_add_int(props, "vhs_tracking_max_interval_s", obs_module_text("VhsTrackingMaxInterval"), 1, 60, 1);
	obs_properties_add_int(props, "vhs_tracking_line_min_count", obs_module_text("VhsTrackingLineMinCount"), 1, VHS_TRACKING_LINE_ARRAY_CAP, 1);
	obs_properties_add_int(props, "vhs_tracking_line_max_count", obs_module_text("VhsTrackingLineMaxCount"), 1, VHS_TRACKING_LINE_ARRAY_CAP, 1);
	obs_properties_add_int(props, "vhs_tracking_line_gap", obs_module_text("VhsTrackingLineGap"), 0, 30, 1);
	obs_properties_add_int(props, "vhs_tracking_min_thickness", obs_module_text("VhsTrackingMinThickness"), 1, 30, 1);
	obs_properties_add_int(props, "vhs_tracking_max_thickness", obs_module_text("VhsTrackingMaxThickness"), 1, 30, 1);
	obs_properties_add_float(props, "vhs_tracking_jitter_min", obs_module_text("VhsTrackingJitterMin"), 0.0, 40.0, 0.5);
	obs_properties_add_float(props, "vhs_tracking_jitter_max", obs_module_text("VhsTrackingJitterMax"), 0.0, 40.0, 0.5);
	obs_properties_add_float(props, "vhs_tracking_brighten", obs_module_text("VhsTrackingBrighten"), 0.0, 1.0, 0.05);
	obs_properties_add_int(props, "vhs_glitch_chance_pct", obs_module_text("VhsGlitchChance"), 0, 100, 1);
	obs_properties_add_int(props, "vhs_glitch_max_bands", obs_module_text("VhsGlitchMaxBands"), 1, 10, 1);

	obs_properties_add_int(props, "eightmm_intensity", obs_module_text("EightMmIntensity"), 0, 100, 1);
	obs_properties_add_float(props, "eightmm_vignette_strength", obs_module_text("EightMmVignetteStrength"), 0.0, 1.0, 0.05);
	obs_properties_add_float(props, "eightmm_warmth", obs_module_text("EightMmWarmth"), 0.0, 80.0, 1.0);
	obs_properties_add_float(props, "eightmm_light_leak_alpha", obs_module_text("EightMmLightLeakAlpha"), 0.0, 1.0, 0.05);
	obs_property_t *eightmm_light_leak_position_prop = obs_properties_add_list(props, "eightmm_light_leak_position", obs_module_text("EightMmLightLeakPosition"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(eightmm_light_leak_position_prop, obs_module_text("EightMmLightLeakPositionNone"), "none");
	obs_property_list_add_string(eightmm_light_leak_position_prop, obs_module_text("EightMmLightLeakPositionTopLeft"), "top_left");
	obs_property_list_add_string(eightmm_light_leak_position_prop, obs_module_text("EightMmLightLeakPositionTopRight"), "top_right");
	obs_property_list_add_string(eightmm_light_leak_position_prop, obs_module_text("EightMmLightLeakPositionBottomLeft"), "bottom_left");
	obs_property_list_add_string(eightmm_light_leak_position_prop, obs_module_text("EightMmLightLeakPositionBottomRight"), "bottom_right");
	obs_properties_add_int(props, "eightmm_light_leak_intensity", obs_module_text("EightMmLightLeakIntensity"), 1, 100, 1);
	obs_properties_add_float(props, "eightmm_weave_px", obs_module_text("EightMmWeave"), 0.0, 15.0, 0.5);
	obs_properties_add_float(props, "eightmm_flicker", obs_module_text("EightMmFlicker"), 0.0, 1.0, 0.05);
	obs_properties_add_int(props, "eightmm_scratch_intensity", obs_module_text("EightMmScratchIntensity"), 0, 100, 1);
	obs_properties_add_int(props, "eightmm_dust_intensity", obs_module_text("EightMmDustIntensity"), 0, 100, 1);
	obs_properties_add_int(props, "eightmm_scratch_max_count", obs_module_text("EightMmScratchMaxCount"), 0, 60, 1);
	obs_properties_add_int(props, "eightmm_dust_max_count", obs_module_text("EightMmDustMaxCount"), 0, 500, 5);

	obs_properties_add_color(props, "duotone_shadow_color", obs_module_text("DuotoneShadowColor"));
	obs_properties_add_color(props, "duotone_highlight_color", obs_module_text("DuotoneHighlightColor"));
	obs_properties_add_int(props, "duotone_intensity", obs_module_text("DuotoneIntensity"), 0, 100, 1);

	obs_properties_add_int(props, "bw_desaturation", obs_module_text("BwDesaturation"), 0, 100, 1);
	obs_properties_add_int(props, "bw_contrast", obs_module_text("BwContrast"), -100, 100, 1);
	obs_properties_add_float(props, "bw_vignette_strength", obs_module_text("BwVignetteStrength"), 0.0, 1.0, 0.05);

	obs_properties_add_int(props, "glitch_intensity", obs_module_text("GlitchIntensity"), 0, 100, 1);
	obs_properties_add_int(props, "glitch_pixel_sort_chance", obs_module_text("GlitchPixelSortChance"), 0, 100, 1);
	obs_properties_add_int(props, "glitch_pixel_sort_max_rows", obs_module_text("GlitchPixelSortMaxRows"), 1, 50, 1);
	obs_properties_add_int(props, "glitch_pixel_sort_threshold", obs_module_text("GlitchPixelSortThreshold"), 0, 100, 1);
	obs_properties_add_int(props, "glitch_tear_chance", obs_module_text("GlitchTearChance"), 0, 100, 1);
	obs_properties_add_int(props, "glitch_tear_max_count", obs_module_text("GlitchTearMaxCount"), 0, 20, 1);
	obs_properties_add_int(props, "glitch_tear_max_height", obs_module_text("GlitchTearMaxHeight"), 1, 200, 1);
	obs_properties_add_int(props, "glitch_tear_max_offset", obs_module_text("GlitchTearMaxOffset"), 0, 400, 1);
	obs_properties_add_int(props, "glitch_tear_duplicate_chance", obs_module_text("GlitchTearDuplicateChance"), 0, 100, 1);
	obs_properties_add_int(props, "glitch_channel_block_chance", obs_module_text("GlitchChannelBlockChance"), 0, 100, 1);
	obs_properties_add_int(props, "glitch_channel_block_max_count", obs_module_text("GlitchChannelBlockMaxCount"), 0, 20, 1);
	obs_properties_add_int(props, "glitch_channel_block_max_size", obs_module_text("GlitchChannelBlockMaxSize"), 2, 400, 1);
	obs_properties_add_int(props, "glitch_channel_block_max_offset", obs_module_text("GlitchChannelBlockMaxOffset"), 0, 200, 1);

	obs_property_set_modified_callback(card_style_prop, card_style_modified);

	obs_property_t *export_settings_prop = obs_properties_add_path(props, "export_settings_path", obs_module_text("ExportSettings"), OBS_PATH_FILE_SAVE, "JSON (*.json)", nullptr);
	obs_property_set_modified_callback(export_settings_prop, export_settings_modified);

	obs_property_t *import_settings_prop = obs_properties_add_path(props, "import_settings_path", obs_module_text("ImportSettings"), OBS_PATH_FILE, "JSON (*.json)", nullptr);
	obs_property_set_modified_callback(import_settings_prop, import_settings_modified);

	//Warning: Do not use a global for this value, as tempting as it is
	//apparently globals are shared between ALL instances of a plugin. Weird.
	obs_property_t *enable_browser_prop = obs_properties_add_bool(props, "enable_browser_media", obs_module_text("EnableBrowserMedia"));
	obs_properties_add_text(props, "enable_browser_media_warning", obs_module_text("EnableBrowserMediaWarning"), OBS_TEXT_INFO);

	auto *ctx = (spotify_source *)data;
	if (ctx && ctx->source) {
		obs_data_t *settings = obs_source_get_settings(ctx->source);
		if (settings) {
			card_style_modified(props, nullptr, settings);
			obs_data_release(settings);
		}
	}
}

static obs_properties_t *spotify_source_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	try {
		spotify_source_properties_impl(props, data);
		return props;
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_properties failed: %s", ex.what());
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_properties failed: Unknown exception");
	}

	//cleanup if fail
	obs_properties_destroy(props);
	return nullptr;
}

static void *spotify_source_create(obs_data_t *settings, obs_source_t *source)
{
	spotify_source *ctx = nullptr;
	try {
		ctx = new spotify_source();
		ctx->source = source;
		apply_settings(ctx, settings);
		InitSourcesLists(ctx);

		auto now = std::chrono::steady_clock::now();
		ctx->autohide_reference_time = now;
		ctx->last_autohide_tick = now;
		ctx->last_playing_time = now;

		ctx->running = true;
		ctx->poll_thread = std::thread(poll_loop, ctx);
		return ctx;
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_create failed: %s", ex.what());
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_create failed: Unknown exception");
	}

	// cleanup if fail
	delete ctx;
	return nullptr;
}

static void spotify_source_destroy(void *data)
{
	auto *ctx = (spotify_source *)data;
	if (!ctx)
		return;

	try {
		ctx->running = false;
		if (ctx->poll_thread.joinable())
			ctx->poll_thread.join();

		if (ctx->texture) {
			ScopedGraphics gfx;
			gs_texture_destroy(ctx->texture);
			ctx->texture = nullptr;
		}
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_destroy failed: %s", ex.what());
		if (ctx->poll_thread.joinable())
			ctx->poll_thread.detach();
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_destroy failed: Unknown exception");
		if (ctx->poll_thread.joinable())
			ctx->poll_thread.detach();
	}

	delete ctx;
}

static void spotify_source_activate(void *data)
{
	// Called when this source becomes part of the live/program output (its scene went live,
	// or a hidden scene item was shown). Resume the SMTC poll.
	auto *ctx = (spotify_source *)data;
	ctx->is_active = true;
}

static void spotify_source_deactivate(void *data)
{
	// Called when this source stops being part of the live/program output. Pause the SMTC
	// poll until it's needed again.
	auto *ctx = (spotify_source *)data;
	ctx->is_active = false;
}

static uint32_t spotify_source_get_width(void *data)
{
	auto *ctx = (spotify_source *)data;
	return ctx->tex_w ? ctx->tex_w : DEFAULT_CARD_W;
}

static uint32_t spotify_source_get_height(void *data)
{
	auto *ctx = (spotify_source *)data;
	return ctx->tex_h ? ctx->tex_h : DEFAULT_CARD_H;
}

static void spotify_source_tick(void *data, float)
{
	auto *ctx = (spotify_source *)data;

	try {
		if (!ctx->new_bitmap_ready)
			return;

		std::vector<uint8_t> pixels;
		uint32_t w = 0, h = 0;
		{
			std::lock_guard<std::mutex> lock(ctx->bitmap_mutex);
			pixels = ctx->pending_pixels;
			w = ctx->pending_w;
			h = ctx->pending_h;
		}
		ctx->new_bitmap_ready = false;

		if (w == 0 || h == 0)
			return;

		{
			ScopedGraphics gfx;
			if (ctx->texture) {
				gs_texture_destroy(ctx->texture);
				ctx->texture = nullptr;
			}
			const uint8_t *data_ptr = pixels.data();
			ctx->texture = gs_texture_create(w, h, GS_BGRA, 1, &data_ptr, 0);
		}

		ctx->tex_w = w;
		ctx->tex_h = h;
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_tick failed: %s", ex.what());
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_tick failed: Unknown exception");
	}
}

static void spotify_source_render(void *data, gs_effect_t *)
{
	auto *ctx = (spotify_source *)data;

	try {
		if (!ctx->texture)
			return;

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, ctx->texture);

		gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		gs_draw_sprite(ctx->texture, 0, ctx->tex_w, ctx->tex_h);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	} catch (const std::exception &ex) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_render failed: %s", ex.what());
	} catch (...) {
		blog(LOG_ERROR, "[spotify_now_playing] spotify_source_render failed: Unknown exception");
	}
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

void spotify_source_register(void)
{
	GdiplusStartupInput gdiInput;
	GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr);

	obs_source_info info = {};
	info.id = "spotify_now_playing_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = spotify_source_get_name;
	info.create = spotify_source_create;
	info.destroy = spotify_source_destroy;
	info.get_width = spotify_source_get_width;
	info.get_height = spotify_source_get_height;
	info.video_tick = spotify_source_tick;
	info.video_render = spotify_source_render;
	info.get_properties = spotify_source_properties;
	info.get_defaults = spotify_source_defaults;
	info.update = spotify_source_update;
	info.activate = spotify_source_activate;
	info.deactivate = spotify_source_deactivate;

	obs_register_source(&info);
}

void spotify_source_unregister(void)
{
	if (g_gdiplusToken) {
		GdiplusShutdown(g_gdiplusToken);
		g_gdiplusToken = 0;
	}
}