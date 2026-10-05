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

#include "gleem-http.hpp"

#include <curl/curl.h>

namespace {

size_t write_body(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

} // namespace

HttpResponse http_request(const std::string &method, const std::string &url, const std::vector<std::string> &headers,
			  const std::string &body, long timeout_ms)
{
	HttpResponse response;

	CURL *curl = curl_easy_init();
	if (!curl) {
		response.error = "Failed to initialise libcurl";
		return response;
	}

	struct curl_slist *header_list = nullptr;
	for (const std::string &header : headers)
		header_list = curl_slist_append(header_list, header.c_str());

	char errbuf[CURL_ERROR_SIZE] = {0};
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms * 2);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "obs-gleem/" GLEEM_PLUGIN_VERSION);
#ifdef _WIN32
	// Use the Windows certificate store, as OBS itself does.
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
	if (header_list)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

	if (method == "POST") {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	} else if (method != "GET") {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
	}

	const CURLcode result = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	curl_easy_cleanup(curl);
	curl_slist_free_all(header_list);

	if (result != CURLE_OK)
		response.error = errbuf[0] ? errbuf : curl_easy_strerror(result);

	return response;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>> &fields)
{
	std::string out;
	for (const auto &[key, value] : fields) {
		char *escaped_key = curl_easy_escape(nullptr, key.c_str(), static_cast<int>(key.size()));
		char *escaped_value = curl_easy_escape(nullptr, value.c_str(), static_cast<int>(value.size()));
		if (!out.empty())
			out += '&';
		out += escaped_key ? escaped_key : "";
		out += '=';
		out += escaped_value ? escaped_value : "";
		curl_free(escaped_key);
		curl_free(escaped_value);
	}
	return out;
}
