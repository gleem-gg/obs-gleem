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

#pragma once

#include "gleem-http.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct IrlDevice {
	std::string uuid;
	std::string name;
	bool online = false;
	// Ingest is receiving the stream, so there is something to pull.
	bool publishing = false;
};

enum class AuthMode {
	// Not signed in.
	None,
	// A rented Gleem OBS machine hands OBS a token in GLEEM_API_TOKEN.
	Environment,
	// Signed in with the OAuth device flow.
	OAuth,
};

struct SignInState {
	enum class Phase { Idle, Starting, Waiting, Done, Failed };

	Phase phase = Phase::Idle;
	std::string user_code;
	std::string verification_uri;
	std::string verification_uri_complete;
	std::string error;
};

/**
 * The plugin's one connection to gleem.gg: who is signed in, their tokens,
 * and the IRL Sidekicks on the account. Every source shares it, so a scene
 * with several Sidekick sources polls the device list once.
 */
class GleemAccount {
public:
	static GleemAccount &instance();

	void load();
	void shutdown();

	std::string base_url() const;
	AuthMode mode() const;
	bool signed_in() const { return mode() != AuthMode::None; }
	std::string account_name() const;
	bool can_sign_in() const { return !client_id().empty(); }

	void start_sign_in();
	void cancel_sign_in();
	SignInState sign_in_state() const;
	void sign_out();

	// The device list, fetched now. Blocks; call it off the render thread.
	bool refresh_devices(std::string &error);
	std::vector<IrlDevice> devices() const;
	std::optional<IrlDevice> device(const std::string &uuid) const;
	bool devices_loaded() const;

	// The SRT URL to pull a device's stream from. Blocks.
	bool playback_url(const std::string &uuid, std::string &url, std::string &error);

	// Sources register while they exist, so the list is only polled while
	// someone needs it.
	void watch();
	void unwatch();

	// Called, from the polling thread, when the device list or the sign-in
	// changed: devices added, renamed, gone on- or offline.
	void set_devices_changed_callback(std::function<void()> callback);

private:
	GleemAccount() = default;

	std::string client_id() const;
	std::string auth_path() const;
	void load_tokens();
	void save_tokens();
	void clear_tokens();

	// The access token to send, refreshed first when it is about to expire.
	std::string access_token(bool force_refresh = false);
	bool refresh_tokens_locked();
	bool store_token_response(const std::string &body);
	void fetch_account_name();

	HttpResponse api_get(const std::string &path);

	void sign_in_loop(uint64_t generation);
	void poll_loop();

	mutable std::mutex mutex_;
	std::string env_token_;
	std::string base_url_;
	std::string access_token_;
	std::string refresh_token_;
	int64_t expires_at_ = 0;
	std::string account_name_;

	// Serialises refreshes: the refresh token rotates on every use.
	std::mutex refresh_mutex_;

	std::vector<IrlDevice> devices_;
	bool devices_loaded_ = false;

	SignInState sign_in_;
	uint64_t sign_in_generation_ = 0;
	std::thread sign_in_thread_;

	std::thread poll_thread_;
	std::condition_variable wake_;
	bool stopping_ = false;
	bool poll_now_ = false;
	int watchers_ = 0;
	std::function<void()> devices_changed_;
};
