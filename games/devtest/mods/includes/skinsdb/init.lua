
skins = {}
skins.modpath = minetest.get_modpath(minetest.get_current_modname())
skins.default = "character"

-- see skindsdb/textures/readme.txt to avoid playername with underscore problem
skins.fsep = minetest.settings:get("skinsdb_fsep") or "_"
if skins.fsep == "_" then
	minetest.log("warning", "skinsdb filename seperator is set to " .. skins.fsep .. ", see skindsdb/textures/readme.txt to avoid problems with playernames containing underscore")
end

dofile(skins.modpath.."/skin_meta_api.lua")
dofile(skins.modpath.."/api.lua")
dofile(skins.modpath.."/skinlist.lua")
dofile(skins.modpath.."/formspecs.lua")
dofile(skins.modpath.."/chatcommands.lua")
-- Unified inventory page/integration
if minetest.get_modpath("unified_inventory") then
	dofile(skins.modpath.."/unified_inventory_page.lua")
end

--if minetest.get_modpath("sfinv") then
--	dofile(skins.modpath.."/sfinv_page.lua")
--end

-- ie.loadfile does not exist?
skins.ie = minetest.request_insecure_environment()
skins.http = minetest.request_http_api()
dofile(skins.modpath.."/skins_updater.lua")
skins.ie = nil
skins.http = nil

-- 3d_armor compatibility
if minetest.global_exists("armor") then
	skins.armor_loaded = true
	armor.get_player_skin = function(self, name)
		print(name);
		local skin = skins.get_player_skin(minetest.get_player_by_name(name))
		return skin:get_texture()
	end
	armor.get_preview = function(self, name)
		local skin = skins.get_player_skin(minetest.get_player_by_name(name))
		return skin:get_preview()
	end
	armor.update_player_visuals = function(self, player)
		if not player then
			return
		end
		local skin = skins.get_player_skin(player)
		skin:apply_skin_to_player(player)
		armor:run_callbacks("on_update", player)
	end
end

if minetest.global_exists("clothing") and clothing.player_textures then
	skins.clothing_loaded = true
	clothing:register_on_update(skins.update_player_skin)
end

-- Update skin on join
skins.ui_context = {}
minetest.register_on_joinplayer(function(player)
	--skins.update_player_skin(player)
end)

minetest.register_on_leaveplayer(function(player)
	skins.ui_context[player:get_player_name()] = nil
	player:get_inventory():set_size("hand", 0)
end)

minetest.register_on_shutdown(function()
	for _, player in pairs(minetest.get_connected_players()) do
		player:get_inventory():set_size("hand", 0)
	end
end)
--]]
--[[
player_api.register_model("character.b3d", {
	animation_speed = 30,
	textures = {
		"character.png",
		"character.png",
		"character.png",
		"character.png"
	},
	animations = {
		stand = {x=0, y=79},
		lay = {x=162, y=166},
		walk = {x=168, y=187},
		mine = {x=189, y=198},
		walk_mine = {x=200, y=219},
		sit = {x=81, y=160},
		--wave = {x = 192, y = 196, override_local = true},
		--point = {x = 196, y = 196, override_local = true},
		--freeze = {x = 205, y = 205, override_local = true},
		sneak = {x = 230, y = 290},
		sneakwalk = {x = 300, y = 340, override_local = true},
		sneakmine = {x = 560, y = 600, override_local = true},
		sneakwalkmine = {x = 350, y = 410, override_local = true},
		wave = {x = 430, y = 470},
		applause = {x = 480, y = 530},
		zombie = {x=1100,y=1130},
		useshield = {x=625,y=645,speed=15, override_local = true},
		useshieldrun = {x=670,y=710, override_local = true},
		useshieldmine = {x=950,y=990, override_local = true},
		useshieldrunmine = {x=1150,y=1190, override_local = true},
		useshieldsneak = {x=740,y=800, override_local = true},
		useshieldsneakmine = {x=1020,y=1060,override_local = true},
		useshieldsneakrun = {x=810,y=850, override_local = true},
		useshieldsneakrunmine = {x=870,y=910, override_local = true},
	},
})

--]]
-- Register default character.png if not part of this mod
local default_skin_obj = skins.get(skins.default)
if not default_skin_obj then
	default_skin_obj = skins.new(skins.default)
	default_skin_obj:set_texture("character.png")
	default_skin_obj:set_meta("format", '1.0')
	default_skin_obj:set_meta("_sort_id", 0)
	default_skin_obj:set_meta("name", "Sam")
	default_skin_obj:set_hand_from_texture()
end

-- Secure hand inventory slot
minetest.register_allow_player_inventory_action(function(player, action, inv, data)
	if data.to_list == "hand" or data.from_list == "hand" or data.listname == "hand" then
		return 0
	end
end)
