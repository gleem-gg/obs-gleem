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
#include "log-redact.hpp"
#include "sidekick-source.hpp"
#include "sign-in-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <curl/curl.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Gleem";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Gleem.Description");
}

bool obs_module_load(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	install_log_redaction();

	GleemAccount::instance().load();
	register_sidekick_source();
	obs_frontend_add_tools_menu_item(
		obs_module_text("Gleem.Menu.Account"), [](void *) { SignInDialog::run(); }, nullptr);

	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	GleemAccount::instance().shutdown();
	remove_log_redaction();
	curl_global_cleanup();
	obs_log(LOG_INFO, "unloaded");
}
