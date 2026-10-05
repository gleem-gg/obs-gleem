/*
 * obs-gleem
 * Copyright (C) 2026 Anikeen UG (haftungsbeschränkt) & Co. KG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "sidekick-source.hpp"

#include "gleem-account.hpp"
#include "sign-in-dialog.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <plugin-support.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

/*
 * "Gleem IRL Sidekick": a Media Source that knows where to find the
 * creator's IRL Sidekick. Decoding is OBS's own Media Source (ffmpeg_source)
 * as a private child, so it behaves exactly like one; this source only picks
 * the device, asks gleem.gg for its pull URL and hands that to the child
 * while ingest has a stream to give. The URL never reaches the saved scene
 * collection: settings hold the device's uuid and name only.
 */

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto tick = std::chrono::seconds(1);
// How long a connection may try before the URL is fetched again: covers a
// creator asking for a new URL and the device moving to another ingest.
constexpr auto connect_grace = std::chrono::seconds(15);
// How long the child keeps reconnecting after ingest stopped publishing,
// before it is told to stop.
constexpr auto linger = std::chrono::seconds(5);
constexpr auto retry_after_error = std::chrono::seconds(10);

enum class Status {
	NoDevice,
	SignedOut,
	Loading,
	NotFound,
	Offline,
	WaitingForStream,
	Connecting,
	Playing,
	Hidden,
	Error,
};

const char *status_key(Status status)
{
	switch (status) {
	case Status::NoDevice:
		return "Gleem.Status.NoDevice";
	case Status::SignedOut:
		return "Gleem.Status.SignedOut";
	case Status::Loading:
		return "Gleem.Status.Loading";
	case Status::NotFound:
		return "Gleem.Status.NotFound";
	case Status::Offline:
		return "Gleem.Status.Offline";
	case Status::WaitingForStream:
		return "Gleem.Status.WaitingForStream";
	case Status::Connecting:
		return "Gleem.Status.Connecting";
	case Status::Playing:
		return "Gleem.Status.Playing";
	case Status::Hidden:
		return "Gleem.Status.Hidden";
	case Status::Error:
		break;
	}
	return "Gleem.Status.Error";
}

struct SidekickSource {
	obs_source_t *source = nullptr;
	obs_source_t *child = nullptr;

	std::mutex mutex;
	std::condition_variable wake;
	bool stopping = false;
	bool reconnect = false;
	std::string uuid;
	bool hw_decode = true;
	bool close_when_hidden = false;
	bool showing = false;
	Status status = Status::NoDevice;
	std::string error;

	std::thread worker;

	// Worker thread only.
	std::string applied_uuid;
	std::string child_input;
	bool child_hw_decode = true;
	Clock::time_point input_set_at;
	Clock::time_point last_playing;
	Clock::time_point next_attempt;

	void set_status(Status next, const std::string &message = std::string())
	{
		std::lock_guard lock(mutex);
		if (status != next || error != message) {
			if (next == Status::Error)
				obs_log(LOG_WARNING, "'%s': %s", obs_source_get_name(source), message.c_str());
			status = next;
			error = message;
		}
	}

	void apply_child(const std::string &input, bool hw)
	{
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", false);
		obs_data_set_string(settings, "input", input.c_str());
		obs_data_set_bool(settings, "hw_decode", hw);
		obs_data_set_bool(settings, "restart_on_activate", false);
		obs_data_set_bool(settings, "close_when_inactive", false);
		obs_data_set_bool(settings, "clear_on_media_end", true);
		obs_data_set_int(settings, "reconnect_delay_sec", 1);
		obs_data_set_int(settings, "buffering_mb", 2);
		// Its settings dump would print the URL, ticket and all.
		obs_data_set_bool(settings, "log_changes", false);
		obs_source_update(child, settings);

		child_input = input;
		child_hw_decode = hw;
		input_set_at = Clock::now();
	}

	void stop_child()
	{
		if (!child_input.empty())
			apply_child("", child_hw_decode);
	}

	void run()
	{
		GleemAccount &account = GleemAccount::instance();

		while (true) {
			std::string want_uuid;
			bool want_hw;
			bool hidden;
			bool force;
			{
				std::unique_lock lock(mutex);
				wake.wait_for(lock, tick, [this] { return stopping || reconnect; });
				if (stopping)
					return;
				want_uuid = uuid;
				want_hw = hw_decode;
				hidden = close_when_hidden && !showing;
				force = reconnect;
				reconnect = false;
			}

			if (want_uuid != applied_uuid || want_hw != child_hw_decode) {
				applied_uuid = want_uuid;
				if (!child_input.empty())
					apply_child("", want_hw);
				child_hw_decode = want_hw;
				next_attempt = Clock::time_point();
			}

			if (want_uuid.empty()) {
				stop_child();
				set_status(Status::NoDevice);
				continue;
			}
			if (!account.signed_in()) {
				stop_child();
				set_status(Status::SignedOut);
				continue;
			}
			if (hidden) {
				stop_child();
				set_status(Status::Hidden);
				continue;
			}

			const Clock::time_point now = Clock::now();
			if (!force && !child_input.empty() &&
			    obs_source_media_get_state(child) == OBS_MEDIA_STATE_PLAYING) {
				last_playing = now;
				set_status(Status::Playing);
				continue;
			}

			if (!account.devices_loaded()) {
				set_status(Status::Loading);
				continue;
			}

			const std::optional<IrlDevice> device = account.device(want_uuid);
			if (!device) {
				stop_child();
				set_status(Status::NotFound);
				continue;
			}

			if (!device->publishing) {
				// Nothing at ingest to pull: stop the child's reconnect
				// attempts rather than have it knock every second.
				if (!child_input.empty() && now - last_playing > linger && now - input_set_at > linger)
					stop_child();
				set_status(device->online ? Status::WaitingForStream : Status::Offline);
				continue;
			}

			const bool stale = !child_input.empty() && now - input_set_at > connect_grace;
			if ((child_input.empty() || stale || force) && now >= next_attempt) {
				std::string url;
				std::string error;
				if (!account.playback_url(want_uuid, url, error)) {
					next_attempt = now + retry_after_error;
					set_status(Status::Error, error);
					continue;
				}
				apply_child(url, want_hw);
			}
			set_status(Status::Connecting);
		}
	}
};

void forward_audio(void *param, obs_source_t *, const struct audio_data *data, bool muted)
{
	if (muted)
		return;

	auto *s = static_cast<SidekickSource *>(param);
	const struct audio_output_info *info = audio_output_get_info(obs_get_audio());

	struct obs_source_audio audio = {};
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		audio.data[i] = data->data[i];
	audio.frames = data->frames;
	audio.timestamp = data->timestamp;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.speakers = info->speakers;
	audio.samples_per_sec = info->samples_per_sec;

	obs_source_output_audio(s->source, &audio);
}

const char *get_name(void *)
{
	return obs_module_text("Gleem.Source.Name");
}

void update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<SidekickSource *>(data);
	{
		std::lock_guard lock(s->mutex);
		s->uuid = obs_data_get_string(settings, "device_uuid");
		s->hw_decode = obs_data_get_bool(settings, "hw_decode");
		s->close_when_hidden = obs_data_get_bool(settings, "close_when_hidden");
	}
	s->wake.notify_all();
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new SidekickSource();
	s->source = source;

	const std::string child_name = std::string(obs_source_get_name(source)) + " (Gleem)";
	OBSDataAutoRelease child_settings = obs_data_create();
	obs_data_set_bool(child_settings, "is_local_file", false);
	obs_data_set_bool(child_settings, "log_changes", false);
	s->child = obs_source_create_private("ffmpeg_source", child_name.c_str(), child_settings);
	if (!s->child) {
		obs_log(LOG_ERROR, "OBS's Media Source (ffmpeg_source) is not available");
		delete s;
		return nullptr;
	}
	obs_source_add_audio_capture_callback(s->child, forward_audio, s);

	update(s, settings);
	GleemAccount::instance().watch();
	s->worker = std::thread(&SidekickSource::run, s);
	return s;
}

void destroy(void *data)
{
	auto *s = static_cast<SidekickSource *>(data);
	{
		std::lock_guard lock(s->mutex);
		s->stopping = true;
	}
	s->wake.notify_all();
	if (s->worker.joinable())
		s->worker.join();

	GleemAccount::instance().unwatch();
	obs_source_remove_audio_capture_callback(s->child, forward_audio, s);
	obs_source_release(s->child);
	delete s;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "hw_decode", true);
	obs_data_set_default_bool(settings, "close_when_hidden", false);
}

bool device_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const std::string uuid = obs_data_get_string(settings, "device_uuid");
	if (const std::optional<IrlDevice> device = GleemAccount::instance().device(uuid))
		obs_data_set_string(settings, "device_name", device->name.c_str());
	return false;
}

bool sign_in_clicked(obs_properties_t *, obs_property_t *, void *)
{
	SignInDialog::run();
	return true;
}

bool refresh_clicked(obs_properties_t *, obs_property_t *, void *)
{
	std::string error;
	if (!GleemAccount::instance().refresh_devices(error))
		obs_log(LOG_WARNING, "Could not load the IRL Sidekicks: %s", error.c_str());
	return true;
}

bool reconnect_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = static_cast<SidekickSource *>(data);
	if (s) {
		{
			std::lock_guard lock(s->mutex);
			s->reconnect = true;
		}
		s->wake.notify_all();
	}
	return false;
}

obs_properties_t *get_properties(void *data)
{
	auto *s = static_cast<SidekickSource *>(data);
	GleemAccount &account = GleemAccount::instance();
	obs_properties_t *props = obs_properties_create();

	// Account.
	switch (account.mode()) {
	case AuthMode::Environment:
		obs_properties_add_text(props, "account", obs_module_text("Gleem.Account.Rental"), OBS_TEXT_INFO);
		break;
	case AuthMode::OAuth: {
		const std::string name = account.account_name();
		std::string label = obs_module_text("Gleem.Account.SignedIn");
		if (!name.empty()) {
			label = obs_module_text("Gleem.Account.SignedInAs");
			const size_t pos = label.find("%1");
			if (pos != std::string::npos)
				label.replace(pos, 2, name);
		}
		obs_properties_add_text(props, "account", label.c_str(), OBS_TEXT_INFO);
		obs_properties_add_button2(props, "sign_in", obs_module_text("Gleem.Account.Manage"), sign_in_clicked,
					   nullptr);
		break;
	}
	case AuthMode::None:
		obs_properties_add_text(props, "account", obs_module_text("Gleem.Account.SignedOut"), OBS_TEXT_INFO);
		obs_properties_add_button2(props, "sign_in", obs_module_text("Gleem.SignIn.Button"), sign_in_clicked,
					   nullptr);
		break;
	}

	// Device.
	obs_property_t *list = obs_properties_add_list(props, "device_uuid", obs_module_text("Gleem.Source.Device"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("Gleem.Source.ChooseDevice"), "");

	std::string selected;
	std::string selected_name;
	if (s) {
		OBSDataAutoRelease settings = obs_source_get_settings(s->source);
		selected = obs_data_get_string(settings, "device_uuid");
		selected_name = obs_data_get_string(settings, "device_name");
	}

	bool selected_listed = false;
	for (const IrlDevice &device : account.devices()) {
		std::string label = device.name.empty() ? device.uuid : device.name;
		if (!device.online)
			label += std::string(" ") + obs_module_text("Gleem.Source.OfflineSuffix");
		obs_property_list_add_string(list, label.c_str(), device.uuid.c_str());
		selected_listed = selected_listed || device.uuid == selected;
	}
	// Keep a saved device selectable while the list loads or after it was
	// removed, so opening the properties never drops it.
	if (!selected.empty() && !selected_listed) {
		const std::string label = (selected_name.empty() ? selected : selected_name) + " " +
					  obs_module_text("Gleem.Source.MissingSuffix");
		obs_property_list_add_string(list, label.c_str(), selected.c_str());
	}
	obs_property_set_modified_callback(list, device_modified);
	obs_property_set_enabled(list, account.signed_in());

	obs_properties_add_button2(props, "refresh", obs_module_text("Gleem.Source.Refresh"), refresh_clicked, nullptr);

	if (s) {
		Status status;
		std::string error;
		{
			std::lock_guard lock(s->mutex);
			status = s->status;
			error = s->error;
		}
		std::string label = std::string(obs_module_text("Gleem.Source.Status")) + " " +
				    (status == Status::Error ? error : obs_module_text(status_key(status)));
		obs_properties_add_text(props, "status", label.c_str(), OBS_TEXT_INFO);
		obs_properties_add_button2(props, "reconnect", obs_module_text("Gleem.Source.Reconnect"),
					   reconnect_clicked, s);
	}

	// Playback.
	obs_properties_add_bool(props, "hw_decode", obs_module_text("Gleem.Source.HardwareDecode"));
	obs_properties_add_bool(props, "close_when_hidden", obs_module_text("Gleem.Source.CloseWhenHidden"));

	obs_property_t *dashboard = obs_properties_add_button2(
		props, "dashboard", obs_module_text("Gleem.Source.Dashboard"), nullptr, nullptr);
	obs_property_button_set_type(dashboard, OBS_BUTTON_URL);
	obs_property_button_set_url(dashboard, (account.base_url() + "/dashboard/irl").data());

	return props;
}

uint32_t get_width(void *data)
{
	return obs_source_get_width(static_cast<SidekickSource *>(data)->child);
}

uint32_t get_height(void *data)
{
	return obs_source_get_height(static_cast<SidekickSource *>(data)->child);
}

void video_render(void *data, gs_effect_t *)
{
	obs_source_video_render(static_cast<SidekickSource *>(data)->child);
}

enum gs_color_space video_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	return obs_source_get_color_space(static_cast<SidekickSource *>(data)->child, count, preferred_spaces);
}

void enum_active_sources(void *data, obs_source_enum_proc_t callback, void *param)
{
	auto *s = static_cast<SidekickSource *>(data);
	callback(s->source, s->child, param);
}

void set_showing(void *data, bool showing)
{
	auto *s = static_cast<SidekickSource *>(data);
	{
		std::lock_guard lock(s->mutex);
		s->showing = showing;
	}
	s->wake.notify_all();
}

void show(void *data)
{
	set_showing(data, true);
}

void hide(void *data)
{
	set_showing(data, false);
}

} // namespace

void register_sidekick_source()
{
	struct obs_source_info info = {};
	info.id = "gleem_irl_sidekick";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = get_name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = get_width;
	info.get_height = get_height;
	info.video_render = video_render;
	info.video_get_color_space = video_get_color_space;
	info.enum_active_sources = enum_active_sources;
	info.show = show;
	info.hide = hide;
	obs_register_source(&info);
}
