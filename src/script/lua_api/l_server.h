/*
Minetest
Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3.0 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include "lua_api/l_base.h"

class ModApiServer : public ModApiBase
{
private:
	// request_shutdown([message], [reconnect])
	static int l_request_shutdown(lua_State *L);

	// get_server_status()
	static int l_get_server_status(lua_State *L);

	// get_server_uptime()
	static int l_get_server_uptime(lua_State *L);

	// get_worldpath()
	static int l_get_worldpath(lua_State *L);

	// is_singleplayer()
	static int l_is_singleplayer(lua_State *L);

	// get_current_modname()
	static int l_get_current_modname(lua_State *L);

	// get_modpath(modname)
	static int l_get_modpath(lua_State *L);

	// get_modnames()
	// the returned list is sorted alphabetically for you
	static int l_get_modnames(lua_State *L);

	// print(text)
	static int l_print(lua_State *L);

	// chat_send_all(text)
	static int l_chat_send_all(lua_State *L);

	// chat_send_player(name, text)
	static int l_chat_send_player(lua_State *L);

	// show_formspec(playername,formname,formspec)
	static int l_show_formspec(lua_State *L);

	// sound_play(spec, parameters)
	static int l_sound_play(lua_State *L);

	// sound_stop(handle)
	static int l_sound_stop(lua_State *L);

	// sound_fade(handle, step, gain)
	static int l_sound_fade(lua_State *L);

	// dynamic_add_media(filepath)
	static int l_dynamic_add_media_raw(lua_State *L);

	// static_add_media(filename, filepath)
	static int l_static_add_media(lua_State *L);

	// get_player_privs(name, text)
	static int l_get_player_privs(lua_State *L);

	// get_player_ip()
	static int l_get_player_ip(lua_State *L);

	// get_player_information(name)
	static int l_get_player_information(lua_State *L);

	// get_ban_list()
	static int l_get_ban_list(lua_State *L);

	// get_ban_description()
	static int l_get_ban_description(lua_State *L);

	// ban_player()
	static int l_ban_player(lua_State *L);

	// unban_player_or_ip()
	static int l_unban_player_or_ip(lua_State *L);

	// kick_player(name, [message]) -> success
	static int l_kick_player(lua_State *L);

	// remove_player(name)
	static int l_remove_player(lua_State *L);

	// notify_authentication_modified(name)
	static int l_notify_authentication_modified(lua_State *L);

	// get_last_run_mod()
	static int l_get_last_run_mod(lua_State *L);

	// set_last_run_mod(modname)
	static int l_set_last_run_mod(lua_State *L);
	
	
	// do_async_callback(func, params, mod_origin)
	static int l_do_async_callback(lua_State *L);

	// register_async_dofile(path)
	static int l_register_async_dofile(lua_State *L);

	// serialize_roundtrip(obj)
	static int l_serialize_roundtrip(lua_State *L);

	// register_mapgen_script(path)
	static int l_register_mapgen_script(lua_State *L);
	
	static int l_register_slave_server(lua_State *L);
	static int l_do_config_of_proxy(lua_State *L);
	static int l_set_address_for_proxy(lua_State *L);
	static int l_connect_player_to_other_server(lua_State *L);
	static int l_run_server(lua_State *L);
	static int l_unregister_server(lua_State *L);
	static int l_register_server(lua_State *L);
	static int l_get_registered_servers_sockets(lua_State *L);
public:
	static void Initialize(lua_State *L, int top);
	static void InitializeAsync(lua_State *L, int top);
};
