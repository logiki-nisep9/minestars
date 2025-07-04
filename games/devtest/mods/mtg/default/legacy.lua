-- mods/default/legacy.lua

-- Horrible stuff to support old code registering falling nodes
-- Don't use this and never do what this does, it's completely wrong!
-- (More specifically, the client and the C++ code doesn't get the group)
function default.register_falling_node(nodename, texture)
	minetest.log("error", debug.traceback())
	minetest.log('error', "WARNING: default.register_falling_node is deprecated")
	if minetest.registered_nodes[nodename] then
		minetest.registered_nodes[nodename].groups.falling_node = 1
	end
end

function default.spawn_falling_node(p, nodename)
	spawn_falling_node(p, nodename)
end

-- Liquids
WATER_ALPHA = minetest.registered_nodes["default:water_source"].alpha
WATER_VISC = minetest.registered_nodes["default:water_source"].liquid_viscosity
LAVA_VISC = minetest.registered_nodes["default:lava_source"].liquid_viscosity
LIGHT_MAX = default.LIGHT_MAX

-- Formspecs
default.gui_suvival_form = default.gui_survival_form
default.gui_bg     = ""
default.gui_bg_img = ""
default.gui_slots  = ""

-- Chests
--default.register_chest = default.chest.register_chest

-- Check for a volume intersecting protection
function default.intersects_protection(minp, maxp, player_name, interval)
	minetest.log("warning", "default.intersects_protection() is " ..
		"deprecated, use minetest.is_area_protected() instead.")
	return minetest.is_area_protected(minp, maxp, player_name, interval)
end
