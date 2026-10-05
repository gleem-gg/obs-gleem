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

#include "gleem-account.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <plugin-support.h>
#include <util/platform.h>

#include <chrono>
#include <cstdlib>
#include <ctime>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

constexpr const char *scope = "irl:read";
constexpr const char *device_grant = "urn:ietf:params:oauth:grant-type:device_code";
constexpr auto poll_interval = std::chrono::seconds(5);
// Refresh this long before the access token runs out.
constexpr int64_t refresh_margin = 60;

std::string env(const char *name)
{
	const char *value = getenv(name);
	return value ? std::string(value) : std::string();
}

std::string trim_trailing_slash(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	return url;
}

int64_t now_seconds()
{
	return static_cast<int64_t>(time(nullptr));
}

// obs_data from a JSON body; nullptr when the body is not a JSON object.
OBSDataAutoRelease parse(const std::string &body)
{
	if (body.empty() || body.front() != '{')
		return nullptr;
	return obs_data_create_from_json(body.c_str());
}

std::string oauth_error(const HttpResponse &response)
{
	OBSDataAutoRelease data = parse(response.body);
	if (!data)
		return {};
	return obs_data_get_string(data, "error");
}

// The Developer API's error message, or a fallback naming the status.
std::string api_error(const HttpResponse &response)
{
	if (response.transport_failed())
		return response.error;

	OBSDataAutoRelease data = parse(response.body);
	if (data) {
		OBSDataAutoRelease error = obs_data_get_obj(data, "error");
		if (error) {
			const char *message = obs_data_get_string(error, "message");
			if (message && *message)
				return message;
		}
	}
	return "HTTP " + std::to_string(response.status);
}

} // namespace

GleemAccount &GleemAccount::instance()
{
	static GleemAccount account;
	return account;
}

void GleemAccount::load()
{
	{
		std::lock_guard lock(mutex_);
		env_token_ = env("GLEEM_API_TOKEN");
		const std::string env_url = env("GLEEM_API_URL");
		base_url_ = trim_trailing_slash(env_url.empty() ? GLEEM_DEFAULT_API_URL : env_url);
		stopping_ = false;
	}

	if (env_token_.empty())
		load_tokens();
	else
		obs_log(LOG_INFO, "Using the Gleem API token this machine provides");

	poll_thread_ = std::thread(&GleemAccount::poll_loop, this);
}

void GleemAccount::shutdown()
{
	cancel_sign_in();
	{
		std::lock_guard lock(mutex_);
		stopping_ = true;
	}
	wake_.notify_all();
	if (poll_thread_.joinable())
		poll_thread_.join();
	if (sign_in_thread_.joinable())
		sign_in_thread_.join();
}

std::string GleemAccount::base_url() const
{
	std::lock_guard lock(mutex_);
	return base_url_;
}

AuthMode GleemAccount::mode() const
{
	std::lock_guard lock(mutex_);
	if (!env_token_.empty())
		return AuthMode::Environment;
	if (!refresh_token_.empty())
		return AuthMode::OAuth;
	return AuthMode::None;
}

std::string GleemAccount::account_name() const
{
	std::lock_guard lock(mutex_);
	return account_name_;
}

std::string GleemAccount::client_id() const
{
	const std::string from_env = env("GLEEM_OAUTH_CLIENT_ID");
	return from_env.empty() ? std::string(GLEEM_OAUTH_CLIENT_ID) : from_env;
}

std::string GleemAccount::auth_path() const
{
	char *path = obs_module_config_path("auth.json");
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

void GleemAccount::load_tokens()
{
	const std::string path = auth_path();
	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data)
		return;

	std::lock_guard lock(mutex_);
	// Tokens belong to the gleem.gg they came from; a build pointed at
	// another one starts signed out.
	if (base_url_ != obs_data_get_string(data, "base_url"))
		return;

	access_token_ = obs_data_get_string(data, "access_token");
	refresh_token_ = obs_data_get_string(data, "refresh_token");
	expires_at_ = obs_data_get_int(data, "expires_at");
	account_name_ = obs_data_get_string(data, "account_name");
}

void GleemAccount::save_tokens()
{
	OBSDataAutoRelease data = obs_data_create();
	{
		std::lock_guard lock(mutex_);
		obs_data_set_string(data, "base_url", base_url_.c_str());
		obs_data_set_string(data, "access_token", access_token_.c_str());
		obs_data_set_string(data, "refresh_token", refresh_token_.c_str());
		obs_data_set_int(data, "expires_at", expires_at_);
		obs_data_set_string(data, "account_name", account_name_.c_str());
	}

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	const std::string path = auth_path();
	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak")) {
		obs_log(LOG_WARNING, "Could not save the Gleem sign-in to %s", path.c_str());
		return;
	}
#ifndef _WIN32
	chmod(path.c_str(), 0600);
	chmod((path + ".bak").c_str(), 0600);
#endif
}

void GleemAccount::clear_tokens()
{
	{
		std::lock_guard lock(mutex_);
		access_token_.clear();
		refresh_token_.clear();
		expires_at_ = 0;
		account_name_.clear();
		devices_.clear();
		devices_loaded_ = false;
	}

	const std::string path = auth_path();
	os_unlink(path.c_str());
	os_unlink((path + ".bak").c_str());
}

bool GleemAccount::store_token_response(const std::string &body)
{
	OBSDataAutoRelease data = parse(body);
	if (!data)
		return false;

	const std::string access = obs_data_get_string(data, "access_token");
	const std::string refresh = obs_data_get_string(data, "refresh_token");
	if (access.empty() || refresh.empty())
		return false;

	int64_t expires_in = obs_data_get_int(data, "expires_in");
	if (expires_in <= 0)
		expires_in = 3600;

	std::lock_guard lock(mutex_);
	access_token_ = access;
	refresh_token_ = refresh;
	expires_at_ = now_seconds() + expires_in;
	return true;
}

std::string GleemAccount::access_token(bool force_refresh)
{
	{
		std::lock_guard lock(mutex_);
		if (!env_token_.empty())
			return env_token_;
		if (refresh_token_.empty())
			return {};
	}

	std::lock_guard refresh_lock(refresh_mutex_);
	{
		std::lock_guard lock(mutex_);
		if (refresh_token_.empty())
			return {};
		if (!force_refresh && expires_at_ - refresh_margin > now_seconds())
			return access_token_;
	}

	if (!refresh_tokens_locked())
		return {};

	std::lock_guard lock(mutex_);
	return access_token_;
}

bool GleemAccount::refresh_tokens_locked()
{
	std::string base;
	std::string refresh;
	{
		std::lock_guard lock(mutex_);
		base = base_url_;
		refresh = refresh_token_;
	}

	const HttpResponse response = http_request(
		"POST", base + "/oauth/token",
		{"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"},
		form_encode({{"grant_type", "refresh_token"}, {"refresh_token", refresh}, {"client_id", client_id()}}));

	if (response.status == 200 && store_token_response(response.body)) {
		save_tokens();
		return true;
	}

	const std::string error = oauth_error(response);
	if (error == "invalid_grant" || error == "invalid_client") {
		// Revoked, expired after six months unused, or the app is gone.
		obs_log(LOG_WARNING, "The Gleem sign-in is no longer valid (%s); signed out", error.c_str());
		clear_tokens();
	} else {
		obs_log(LOG_WARNING, "Could not refresh the Gleem sign-in: %s",
			response.transport_failed() ? response.error.c_str() : error.c_str());
	}
	return false;
}

HttpResponse GleemAccount::api_get(const std::string &path)
{
	std::string token = access_token();
	if (token.empty()) {
		HttpResponse response;
		response.status = 401;
		return response;
	}

	const std::string url = base_url() + path;
	HttpResponse response =
		http_request("GET", url, {"Accept: application/json", "Authorization: Bearer " + token});

	// The token may have been revoked or rotated elsewhere: one fresh try.
	if (response.status == 401 && mode() == AuthMode::OAuth) {
		token = access_token(true);
		if (!token.empty())
			response = http_request("GET", url,
						{"Accept: application/json", "Authorization: Bearer " + token});
	}
	return response;
}

void GleemAccount::fetch_account_name()
{
	const HttpResponse response = api_get("/api/v1/me");
	OBSDataAutoRelease data = response.status == 200 ? parse(response.body) : nullptr;
	if (!data)
		return;

	OBSDataAutoRelease creator = obs_data_get_obj(data, "creator");
	if (!creator)
		return;

	std::lock_guard lock(mutex_);
	account_name_ = obs_data_get_string(creator, "display_name");
}

void GleemAccount::start_sign_in()
{
	cancel_sign_in();
	if (sign_in_thread_.joinable())
		sign_in_thread_.join();

	uint64_t generation;
	{
		std::lock_guard lock(mutex_);
		generation = ++sign_in_generation_;
		sign_in_ = SignInState();
		sign_in_.phase = SignInState::Phase::Starting;
	}
	sign_in_thread_ = std::thread(&GleemAccount::sign_in_loop, this, generation);
}

void GleemAccount::cancel_sign_in()
{
	{
		std::lock_guard lock(mutex_);
		sign_in_generation_++;
		if (sign_in_.phase == SignInState::Phase::Starting || sign_in_.phase == SignInState::Phase::Waiting)
			sign_in_.phase = SignInState::Phase::Idle;
	}
	wake_.notify_all();
}

SignInState GleemAccount::sign_in_state() const
{
	std::lock_guard lock(mutex_);
	return sign_in_;
}

void GleemAccount::sign_in_loop(uint64_t generation)
{
	const std::string base = base_url();
	const std::string client = client_id();

	auto fail = [&](const std::string &message) {
		std::lock_guard lock(mutex_);
		if (sign_in_generation_ != generation)
			return;
		sign_in_.phase = SignInState::Phase::Failed;
		sign_in_.error = message;
	};

	const HttpResponse start =
		http_request("POST", base + "/oauth/device_authorization",
			     {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"},
			     form_encode({{"client_id", client}, {"scope", scope}}));
	OBSDataAutoRelease started = start.status == 200 ? parse(start.body) : nullptr;
	if (!started) {
		obs_log(LOG_WARNING, "Gleem sign-in could not start: %s",
			start.transport_failed() ? start.error.c_str() : start.body.c_str());
		fail(start.transport_failed() ? start.error : obs_module_text("Gleem.SignIn.StartFailed"));
		return;
	}

	const std::string device_code = obs_data_get_string(started, "device_code");
	int64_t interval = obs_data_get_int(started, "interval");
	if (interval < 1)
		interval = 5;
	int64_t expires_in = obs_data_get_int(started, "expires_in");
	if (expires_in < 1)
		expires_in = 600;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires_in);

	{
		std::lock_guard lock(mutex_);
		if (sign_in_generation_ != generation)
			return;
		sign_in_.phase = SignInState::Phase::Waiting;
		sign_in_.user_code = obs_data_get_string(started, "user_code");
		sign_in_.verification_uri = obs_data_get_string(started, "verification_uri");
		sign_in_.verification_uri_complete = obs_data_get_string(started, "verification_uri_complete");
	}

	while (true) {
		{
			std::unique_lock lock(mutex_);
			wake_.wait_for(lock, std::chrono::seconds(interval),
				       [&] { return stopping_ || sign_in_generation_ != generation; });
			if (stopping_ || sign_in_generation_ != generation)
				return;
		}

		if (std::chrono::steady_clock::now() > deadline) {
			fail(obs_module_text("Gleem.SignIn.Expired"));
			return;
		}

		const HttpResponse poll = http_request(
			"POST", base + "/oauth/token",
			{"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"},
			form_encode(
				{{"grant_type", device_grant}, {"device_code", device_code}, {"client_id", client}}));

		if (poll.transport_failed())
			continue;

		if (poll.status == 200) {
			if (!store_token_response(poll.body)) {
				fail(obs_module_text("Gleem.SignIn.StartFailed"));
				return;
			}
			fetch_account_name();
			save_tokens();
			{
				std::lock_guard lock(mutex_);
				if (sign_in_generation_ == generation)
					sign_in_.phase = SignInState::Phase::Done;
				poll_now_ = true;
			}
			obs_log(LOG_INFO, "Signed in to Gleem as %s", account_name().c_str());
			wake_.notify_all();
			return;
		}

		const std::string error = oauth_error(poll);
		if (error == "authorization_pending")
			continue;
		if (error == "slow_down") {
			interval += 5;
			continue;
		}
		if (error == "access_denied") {
			fail(obs_module_text("Gleem.SignIn.Denied"));
			return;
		}
		if (error == "expired_token") {
			fail(obs_module_text("Gleem.SignIn.Expired"));
			return;
		}

		obs_log(LOG_WARNING, "Gleem sign-in failed: HTTP %ld %s", poll.status, error.c_str());
		fail(obs_module_text("Gleem.SignIn.StartFailed"));
		return;
	}
}

void GleemAccount::sign_out()
{
	cancel_sign_in();

	std::string refresh;
	std::string base;
	{
		std::lock_guard lock(mutex_);
		if (!env_token_.empty())
			return;
		refresh = refresh_token_;
		base = base_url_;
	}

	if (!refresh.empty()) {
		// Revoking the refresh token ends the whole session on gleem.gg.
		http_request("POST", base + "/oauth/revoke", {"Content-Type: application/x-www-form-urlencoded"},
			     form_encode({{"token", refresh}, {"client_id", client_id()}}), 5000);
	}

	clear_tokens();
	{
		std::lock_guard lock(mutex_);
		sign_in_ = SignInState();
	}
	obs_log(LOG_INFO, "Signed out of Gleem");
}

bool GleemAccount::refresh_devices(std::string &error)
{
	if (!signed_in()) {
		error = obs_module_text("Gleem.Status.SignedOut");
		return false;
	}

	const HttpResponse response = api_get("/api/v1/irl/devices");
	if (response.status != 200) {
		error = api_error(response);
		return false;
	}

	OBSDataAutoRelease data = parse(response.body);
	OBSDataArrayAutoRelease list = data ? obs_data_get_array(data, "data") : nullptr;
	if (!list) {
		error = "Unexpected answer from gleem.gg";
		return false;
	}

	std::vector<IrlDevice> devices;
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		OBSDataAutoRelease item = obs_data_array_item(list, i);
		IrlDevice device;
		device.uuid = obs_data_get_string(item, "uuid");
		device.name = obs_data_get_string(item, "name");
		device.online = obs_data_get_bool(item, "online");
		OBSDataAutoRelease ingest = obs_data_get_obj(item, "ingest");
		device.publishing = ingest && obs_data_get_bool(ingest, "publishing");
		if (!device.uuid.empty())
			devices.push_back(std::move(device));
	}

	std::lock_guard lock(mutex_);
	devices_ = std::move(devices);
	devices_loaded_ = true;
	return true;
}

std::vector<IrlDevice> GleemAccount::devices() const
{
	std::lock_guard lock(mutex_);
	return devices_;
}

std::optional<IrlDevice> GleemAccount::device(const std::string &uuid) const
{
	std::lock_guard lock(mutex_);
	for (const IrlDevice &device : devices_)
		if (device.uuid == uuid)
			return device;
	return std::nullopt;
}

bool GleemAccount::devices_loaded() const
{
	std::lock_guard lock(mutex_);
	return devices_loaded_;
}

bool GleemAccount::playback_url(const std::string &uuid, std::string &url, std::string &error)
{
	// The uuid comes from a scene collection file; keep it to a uuid's
	// characters before it goes into a path.
	if (uuid.empty() || uuid.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos) {
		error = obs_module_text("Gleem.Status.NotFound");
		return false;
	}

	const HttpResponse response = api_get("/api/v1/irl/devices/" + uuid + "/playback");
	OBSDataAutoRelease data = response.status == 200 ? parse(response.body) : nullptr;
	if (!data) {
		error = api_error(response);
		return false;
	}

	url = obs_data_get_string(data, "url");
	if (url.rfind("srt://", 0) != 0) {
		error = "Unexpected answer from gleem.gg";
		return false;
	}
	return true;
}

void GleemAccount::watch()
{
	{
		std::lock_guard lock(mutex_);
		watchers_++;
		poll_now_ = true;
	}
	wake_.notify_all();
}

void GleemAccount::unwatch()
{
	std::lock_guard lock(mutex_);
	watchers_--;
}

void GleemAccount::poll_loop()
{
	std::string last_error;

	while (true) {
		bool wanted;
		{
			std::unique_lock lock(mutex_);
			if (stopping_)
				return;
			poll_now_ = false;
			wanted = watchers_ > 0 && (!env_token_.empty() || !refresh_token_.empty());
		}

		if (wanted) {
			std::string error;
			if (refresh_devices(error)) {
				last_error.clear();
			} else if (error != last_error) {
				obs_log(LOG_WARNING, "Could not load the IRL Sidekicks: %s", error.c_str());
				last_error = error;
			}
		}

		std::unique_lock lock(mutex_);
		wake_.wait_for(lock, poll_interval, [&] { return stopping_ || poll_now_; });
	}
}
