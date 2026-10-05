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

#include "log-redact.hpp"

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

log_handler_t previous_handler = nullptr;
void *previous_param = nullptr;

constexpr const char *marker = ":play:";

bool is_ticket_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
	       c == '.';
}

void call_previous(int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	previous_handler(level, format, args, previous_param);
	va_end(args);
}

void redacting_handler(int level, const char *format, va_list args, void *)
{
	char buffer[8192];
	va_list copy;
	va_copy(copy, args);
	vsnprintf(buffer, sizeof(buffer), format, copy);
	va_end(copy);

	if (!strstr(buffer, marker)) {
		previous_handler(level, format, args, previous_param);
		return;
	}

	call_previous(level, "%s", redact_play_tickets(buffer).c_str());
}

} // namespace

std::string redact_play_tickets(const std::string &text)
{
	std::string out;
	out.reserve(text.size());
	const size_t marker_length = strlen(marker);
	size_t pos = 0;

	while (true) {
		const size_t found = text.find(marker, pos);
		if (found == std::string::npos) {
			out.append(text, pos, std::string::npos);
			return out;
		}

		size_t end = found + marker_length;
		out.append(text, pos, end - pos);
		while (end < text.size() && is_ticket_char(text[end]))
			end++;
		out.append("[redacted]");
		pos = end;
	}
}

void install_log_redaction()
{
	log_handler_t handler = nullptr;
	void *param = nullptr;
	base_get_log_handler(&handler, &param);

	if (!handler || handler == redacting_handler)
		return;

	previous_handler = handler;
	previous_param = param;
	base_set_log_handler(redacting_handler, nullptr);
}

void remove_log_redaction()
{
	log_handler_t handler = nullptr;
	void *param = nullptr;
	base_get_log_handler(&handler, &param);

	// Only hand back what we wrapped; someone may have replaced us since.
	if (handler == redacting_handler && previous_handler)
		base_set_log_handler(previous_handler, previous_param);
}
