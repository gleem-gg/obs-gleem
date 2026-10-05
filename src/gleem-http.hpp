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

#include <string>
#include <utility>
#include <vector>

struct HttpResponse {
	long status = 0;
	std::string body;
	// Set when no HTTP answer arrived at all (DNS, TLS, timeout).
	std::string error;

	bool transport_failed() const { return !error.empty(); }
};

HttpResponse http_request(const std::string &method, const std::string &url, const std::vector<std::string> &headers,
			  const std::string &body = std::string(), long timeout_ms = 10000);

// application/x-www-form-urlencoded, as the OAuth endpoints take it.
std::string form_encode(const std::vector<std::pair<std::string, std::string>> &fields);
