core.log("action", "Initializing asynchronous environment (game)")

local function pack2(...)
	return {n=select('#', ...), ...}
end

-- Entrypoint to run async jobs, called by C++
function core.job_processor(func, params)
	--print(params)
	local retval = pack2(func(unpack(params, 1, params.n)))

	return retval
end

--a test
--print(core.set_node_async)
--this was made for abms running on different threads
if core.get_node_light_async then
	local funcs = {
		--"set_node",
		"get_node_light",
		"is_protected",
		"find_node_near",
		"sound_play", --summoned by default
		--"get_node",
		"find_nodes_in_area_under_air",
		"find_nodes_in_area",
		"get_node_timer",
		"get_node_or_nil",
		--"swap_node",
	}
	for _, n in pairs(funcs) do
		core[n] = core[n.."_async"] --registered in this mode for no conflicts between ModApiServer and ModApiEnv
		core.log("action", "Registering "..n.." for Async Environment")
	end
end

--Some obvious function
core.remove_node = function(pos)
	return core.set_node(pos, {name = "air"})
end

-- Import a bunch of individual files from builtin/game/
local gamepath = core.get_builtin_path() .. "game" .. DIR_DELIM

dofile(gamepath .. "constants.lua")
dofile(gamepath .. "item_s.lua")
dofile(gamepath .. "misc_s.lua")
dofile(gamepath .. "features.lua")
dofile(gamepath .. "voxelarea.lua")

-- Transfer of globals
do
	local all = assert(core.transferred_globals)
	--local all = core.deserialize(core.transferred_globals, true)
	core.transferred_globals = nil

	-- reassemble other tables
	all.registered_nodes = {}
	all.registered_craftitems = {}
	all.registered_tools = {}
	for k, v in pairs(all.registered_items) do
		if v.type == "node" then
			all.registered_nodes[k] = v
		elseif v.type == "craftitem" then
			all.registered_craftitems[k] = v
		elseif v.type == "tool" then
			all.registered_tools[k] = v
		end
	end

	for k, v in pairs(all) do
		core[k] = v
	end
end