function core.hash_node_position(pos)
	return (pos.z + 32768) * 65536 * 65536
		 + (pos.y + 32768) * 65536
		 +  pos.x + 32768
end


function core.get_position_from_hash(hash)
	local x = (hash % 65536) - 32768
	hash  = math.floor(hash / 65536)
	local y = (hash % 65536) - 32768
	hash  = math.floor(hash / 65536)
	local z = (hash % 65536) - 32768
	return vector.new(x, y, z)
end


function core.get_item_group(name, group)
	if not core.registered_items[name] or not
			core.registered_items[name].groups[group] then
		return 0
	end
	return core.registered_items[name].groups[group]
end


function core.get_node_group(name, group)
	core.log("deprecated", "Deprecated usage of get_node_group, use get_item_group instead")
	return core.get_item_group(name, group)
end


function core.setting_get_pos(name)
	local value = core.settings:get(name)
	if not value then
		return nil
	end
	return core.string_to_pos(value)
end


-- See l_env.cpp for the other functions
function core.get_artificial_light(param1)
	return math.floor(param1 / 16)
end