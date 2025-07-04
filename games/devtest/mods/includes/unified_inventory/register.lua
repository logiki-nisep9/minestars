local S = unified_inventory.gettext
local F = minetest.formspec_escape

minetest.register_privilege("creative", {
	description = S("Can use the creative inventory"),
	give_to_singleplayer = false,
})

minetest.register_privilege("ui_full", {
	description = S("Forces Unified Inventory to be displayed in Full mode if Lite mode is configured globally"),
	give_to_singleplayer = false,
})


local trash = minetest.create_detached_inventory("trash", {
	--allow_put = function(inv, listname, index, stack, player)
	--	if unified_inventory.is_creative(player:get_player_name()) then
	--		return stack:get_count()
	--	else
	--		return 0
	--	end
	--end,
	on_put = function(inv, listname, index, stack, player)
		inv:set_stack(listname, index, nil)
		local player_name = player:get_player_name()
		minetest.sound_play("trash", {to_player=player_name, gain = 1.0})
	end,
})
trash:set_size("main", 1)

unified_inventory.register_button("craft", {
	type = "image",
	image = "ui_craft_icon.png",
	tooltip = S("Crafting Grid")
})

unified_inventory.register_button("craftguide", {
	type = "image",
	image = "ui_craftguide_icon.png",
	tooltip = S("Crafting Guide")
})

unified_inventory.register_button("home_gui_set", {
	type = "image",
	image = "ui_sethome_icon.png",
	tooltip = S("Set home position"),
	hide_lite=true,
	action = function(player)
		local player_name = player:get_player_name()
		if minetest.check_player_privs(player_name, {home=true}) then
			unified_inventory.set_home(player, player:getpos())
			local home = unified_inventory.home_pos[player_name]
			if home ~= nil then
				minetest.sound_play("dingdong",
						{to_player=player_name, gain = 1.0})
				minetest.chat_send_player(player_name,
					S("Home position set to: %s"):format(minetest.pos_to_string(home)))
			end
		else
			minetest.chat_send_player(player_name,
				S("You don't have the \"home\" privilege!"))
			unified_inventory.set_inventory_formspec(player, unified_inventory.current_page[player_name])
		end
	end,
	condition = function(player)
		return minetest.check_player_privs(player:get_player_name(), {home=true})
	end,
})

unified_inventory.register_button("home_gui_go", {
	type = "image",
	image = "ui_gohome_icon.png",
	tooltip = S("Go home"),
	hide_lite=true,
	action = function(player)
		local player_name = player:get_player_name()
		if minetest.check_player_privs(player_name, {home=true}) then
			minetest.sound_play("teleport",
				{to_player=player:get_player_name(), gain = 1.0})
			unified_inventory.go_home(player)
		else
			minetest.chat_send_player(player_name,
				S("You don't have the \"home\" privilege!"))
			unified_inventory.set_inventory_formspec(player, unified_inventory.current_page[player_name])
		end
	end,
	condition = function(player)
		return minetest.check_player_privs(player:get_player_name(), {home=true})
	end,
})

unified_inventory.register_button("clear_inv", {
	type = "image",
	image = "ui_trash_icon.png",
	tooltip = S("Clear inventory"),
	action = function(player)
		local player_name = player:get_player_name()
		if not unified_inventory.is_creative(player_name) then
			minetest.chat_send_player(player_name,
					S("This button has been disabled outside"
					.." of creative mode to prevent"
					.." accidental inventory trashing."
					.."\nUse the trash slot instead."))
			unified_inventory.set_inventory_formspec(player, unified_inventory.current_page[player_name])
			return
		end
		player:get_inventory():set_list("main", {})
		minetest.chat_send_player(player_name, S('Inventory cleared!'))
		minetest.sound_play("trash_all",
				{to_player=player_name, gain = 1.0})
	end,
	condition = function(player)
		return unified_inventory.is_creative(player:get_player_name())
	end,
})

unified_inventory.register_page("craft", {
	get_formspec = function(player, perplayer_formspec)

		local formspecy = perplayer_formspec.formspec_y
		local formheadery =  perplayer_formspec.form_header_y

		local player_name = player:get_player_name()
		--Main
		local formspec = "background[4,3.3;6,3;ui_crafting_form.png]"
		formspec = formspec.."background[0,"..(formspecy + 6.3)..";8,4;ui_main_inventory.png]"
		formspec = formspec.."listcolors[#10101010;#00000000]"
		formspec = formspec.."list[current_player;craftpreview;8,3.3;1,1;]"
		formspec = formspec.."list[current_player;craft;4,3.3;3,3;]"
		formspec = formspec.."listring[current_name;craft]"
		formspec = formspec.."listring[current_player;main]"
		--Offhand
		formspec = formspec.."label[8.3,"..(formspecy + 5.9)..";" .. F(S("Offhand:")) .. "]"
		formspec = formspec.."list[detached:"..player:get_player_name().."_offhand;offhand;8.5,"..(formspecy + 6.3)..";1,1;]"
		formspec = formspec.."background[8.5,"..(formspecy + 6.3)..";1,1;ui_single_slot.png]"
		--Trash
		formspec = formspec.."label[8.4,"..(formspecy + 7.9)..";" .. F(S("Trash:")) .. "]"
		formspec = formspec.."list[detached:trash;main;8.5,"..(formspecy + 8.3)..";1,1;]"
		formspec = formspec.."background[8.5,"..(formspecy + 8.3)..";1,1;ui_single_slot.png]"
		--Armor
		formspec = formspec.."list[detached:"..player:get_player_name().."_armor;armor;0,1;1,6]"
		formspec = formspec.."background[0,"..(formspecy + 0)..";1,1;ui_single_slot.png]"
		formspec = formspec.."background[0,"..(formspecy + 1)..";1,1;ui_single_slot.png]"
		formspec = formspec.."background[0,"..(formspecy + 2)..";1,1;ui_single_slot.png]"
		formspec = formspec.."background[0,"..(formspecy + 3)..";1,1;ui_single_slot.png]"
		formspec = formspec.."background[0,"..(formspecy + 4)..";1,1;ui_single_slot.png]"
		formspec = formspec.."background[0,"..(formspecy + 5)..";1,1;ui_single_slot.png]"
		--Old format
		--[[
		
		local image = skin:get_texture()
		if m_format == "1.8" then
			formspec = formspec.."label[2,"..(formspecy + 1.2)..";" .. player:get_player_name() .. "]"
			formspec = formspec.."model[1.2,3;4,5;;skinsdb_3d_armor_character_5.b3d;blank.png," .. image .. ",blank.png,blank.png;-20,-150;false;false;0,45]"
		else
			formspec = formspec.."label[2,"..(formspecy + 1.2)..";" .. player:get_player_name() .. "]"
			formspec = formspec.."model[1.2,3;4,5;;skinsdb_3d_armor_character_5.b3d;" .. image .. ",blank.png,blank.png,blank.png;-20,-150;false;false;0,45]"
		end
		--]]
		--Player
		local p_p = player:get_properties()
		--Handled by heads_entity.lua
		--[[local png1 = (p_p.textures[1] ~= "" and p_p.textures[1]) or "character.png" -- Skin ~ 1.8
		local png2 = (p_p.textures[2] ~= "" and p_p.textures[2]) or "blank.png" -- Skin 1.8
		local png3 = (p_p.textures[3] ~= "" and p_p.textures[3]) -- Armor--]]
		
		local skin = skins.get_player_skin(player)
		local png1 = skin:get_texture()
		local png3 = armor:get_textures(player) and armor:get_textures(player).armor
		local png2 = "blank.png"
		
		local png4 = (p_p.textures[4] ~= "" and p_p.textures[4]) or "blank.png" -- Wield Item
		if not MineStars.OldClients[player:get_player_name()] then
			--formspec = formspec .. "label[2,"..(formspecy + 1.2)..";" .. player:get_player_name() .. "]"
			formspec = formspec .. "model[1.2,3;4,5;;skinsdb_3d_armor_character_5.b3d;"..png1..","..png2..","..png3..","..png4..";-20,-150;false;false;0,45]"
		else
			local texture = MineStars.GetPreviewFromTextures(skin:get_preview(), png3, skin:get_texture())
			--print(texture)
			formspec = formspec .. "image[1.2,2.6;2.6,4.75;"..texture.."]"
			--Check shield
			local bool, stack = MineStars.Shield.HasShield(player)
			if bool then
				formspec = formspec .. "image[2.6,4.4;1.1,1.1;"..(stack:get_definition().inventory_image).."]"
			end
		end
		return {formspec=formspec}
	end,
})

-- stack_image_button(): generate a form button displaying a stack of items
--
-- The specified item may be a group.  In that case, the group will be
-- represented by some item in the group, along with a flag indicating
-- that it's a group.  If the group contains only one item, it will be
-- treated as if that item had been specified directly.

local function stack_image_button(x, y, w, h, buttonname_prefix, item)
	local name = item:get_name()
	local count = item:get_count()
	local show_is_group = false
	local displayitem = name.." "..count
	local selectitem = name
	if name:sub(1, 6) == "group:" then
		local group_name = name:sub(7)
		local group_item = unified_inventory.get_group_item(group_name)
		show_is_group = not group_item.sole
		displayitem = group_item.item or "unknown"
		selectitem = group_item.sole and displayitem or name
	end
	local label = show_is_group and "G" or ""
	local buttonname = F(buttonname_prefix..unified_inventory.mangle_for_formspec(selectitem))
	local button = string.format("item_image_button[%f,%f;%f,%f;%s;%s;%s]",
			x, y, w, h,
			F(displayitem), buttonname, label)
	if show_is_group then
		local groupstring, andcount = unified_inventory.extract_groupnames(name)
		local grouptip
		if andcount == 1 then
			grouptip = string.format(S("Any item belonging to the %s group"), groupstring)
		elseif andcount > 1 then
			grouptip = string.format(S("Any item belonging to the groups %s"), groupstring)
		end
		grouptip = F(grouptip)
		if andcount >= 1 then
			button = button  .. string.format("tooltip[%s;%s]", buttonname, grouptip)
		end
	end
	return button
end

local recipe_text = {
	recipe = S("Recipe %d of %d"),
	usage = S("Usage %d of %d"),
}
local no_recipe_text = {
	recipe = S("No recipes"),
	usage = S("No usages"),
}
local role_text = {
	recipe = S("Result"),
	usage = S("Ingredient"),
}
local next_alt_text = {
	recipe = S("Show next recipe"),
	usage = S("Show next usage"),
}
local prev_alt_text = {
	recipe = S("Show previous recipe"),
	usage = S("Show previous usage"),
}
local other_dir = {
	recipe = "usage",
	usage = "recipe",
}

unified_inventory.register_page("craftguide", {
	get_formspec = function(player, perplayer_formspec)

		local formspecy =    perplayer_formspec.formspec_y
		local formheadery =  perplayer_formspec.form_header_y
		local craftresultx = perplayer_formspec.craft_result_x
		local craftresulty = perplayer_formspec.craft_result_y

		local player_name = player:get_player_name()
		local player_privs = minetest.get_player_privs(player_name)
		local formspec = ""
		formspec = formspec.."background[0,"..(formspecy + 6.3)..";8,4;ui_main_inventory.png]"
		formspec = formspec.."label[0,"..(formheadery + 0.8)..";" .. F(S("Crafting Guide")) .. "]"
		formspec = formspec.."listcolors[#00000000;#00000000]"
		local item_name = unified_inventory.current_item[player_name]
		if not item_name then return {formspec=formspec} end
		local item_name_shown
		if minetest.registered_items[item_name] and minetest.registered_items[item_name].description then
			item_name_shown = string.format(S("%s (%s)"), minetest.registered_items[item_name].description, item_name)
		else
			item_name_shown = item_name
		end

		local dir = unified_inventory.current_craft_direction[player_name]
		local rdir
		if dir == "recipe" then rdir = "usage" end
		if dir == "usage" then rdir = "recipe" end
		local crafts = unified_inventory.crafts_for[dir][item_name]
		local alternate = unified_inventory.alternate[player_name]
		local alternates, craft
		if crafts ~= nil and #crafts > 0 then
			alternates = #crafts
			craft = crafts[alternate]
		end
		local has_creative = player_privs.give or player_privs.creative or
			minetest.settings:get_bool("creative_mode")

		formspec = formspec.."background[0.5,"..(formspecy + 2.2)..";8,3;ui_craftguide_form.png]"
		formspec = formspec.."textarea["..(craftresultx - 0.2)..","..(craftresulty + 5)
                           ..";10,1;;"..F(role_text[dir])..": "..item_name_shown..";]"
		formspec = formspec..stack_image_button(0, (formspecy + 1), 1.1, 1.1, "item_button_"
		                   .. rdir .. "_", ItemStack(item_name))

		if not craft then
			formspec = formspec.."label[6.4,"..(formspecy + 1.7)..";"
			                   ..F(no_recipe_text[dir]).."]"
			local no_pos = dir == "recipe" and 4.5 or 6.5
			local item_pos = dir == "recipe" and 6.5 or 4.5
			formspec = formspec.."image["..no_pos..","..(formspecy + 2)..";1.1,1.1;ui_no.png]"
			formspec = formspec..stack_image_button(item_pos, (formspecy + 2.2), 1.1, 1.1, "item_button_"
			                   ..other_dir[dir].."_", ItemStack(item_name))
			if has_creative then
				formspec = formspec.."label[0,"..(formspecy + 3.10)..";" .. F(S("Give me:")) .. "]"
						.."button[0,  "..(formspecy + 3.7)..";0.6,0.5;craftguide_giveme_1;1]"
						.."button[0.6,"..(formspecy + 3.7)..";0.7,0.5;craftguide_giveme_10;10]"
						.."button[1.3,"..(formspecy + 3.7)..";0.8,0.5;craftguide_giveme_99;99]"
			end
			return {formspec = formspec}
		end

		local craft_type = unified_inventory.registered_craft_types[craft.type] or
				unified_inventory.craft_type_defaults(craft.type, {})
		if craft_type.icon then
			formspec = formspec..string.format(" image[%f,%f;%f,%f;%s]",5.7,(formspecy + 1.25),0.5,0.5,craft_type.icon)
		end
		formspec = formspec.."label[6.4,"..(formspecy + 1.7)..";" .. F(craft_type.description).."]"
		formspec = formspec..stack_image_button(6.5, (formspecy + 2.2), 1.1, 1.1, "item_button_usage_", ItemStack(craft.output))
		local display_size = craft_type.dynamic_display_size and craft_type.dynamic_display_size(craft) or { width = craft_type.width, height = craft_type.height }
		local craft_width = craft_type.get_shaped_craft_width and craft_type.get_shaped_craft_width(craft) or display_size.width

		--hack
		local fpy = formspecy
		formspecy = formspecy + 1.3

		-- This keeps recipes aligned to the right,
		-- so that they're close to the arrow.
		local xoffset = 5.5
		-- Offset factor for crafting grids with side length > 4
		local of = (3/math.max(3, math.max(display_size.width, display_size.height)))
		local od = 0
		-- Minimum grid size at which size optimazation measures kick in
		local mini_craft_size = 6
		if display_size.width >= mini_craft_size then
			od = math.max(1, display_size.width - 2)
			xoffset = xoffset - 0.1
		end
		-- Size modifier factor
		local sf = math.min(1, of * (1.05 + 0.05*od))
		-- Button size
		local bsize_h = 1.1 * sf
		local bsize_w = bsize_h
		if display_size.width >= mini_craft_size then
			bsize_w = 1.175 * sf
		end
		if (bsize_h > 0.35 and display_size.width) then
		for y = 1, display_size.height do
		for x = 1, display_size.width do
			local item
			if craft and x <= craft_width then
				item = craft.items[(y-1) * craft_width + x]
			end
			-- Flipped x, used to build formspec buttons from right to left
			local fx = display_size.width - (x-1)
			-- x offset, y offset
			local xof = (fx-1) * of + of
			local yof = (y-1) * of + 1
			if item then
				formspec = formspec..stack_image_button(
						xoffset - xof, formspecy - 1 + yof, bsize_w, bsize_h,
						"item_button_recipe_",
						ItemStack(item))
			else
				-- Fake buttons just to make grid
				formspec = formspec.."image_button["
					..tostring(xoffset - xof)..","..tostring(formspecy - 1 + yof)
					..";"..bsize_w..","..bsize_h..";ui_blank_image.png;;]"
			end
		end
		end
		else
			-- Error
			formspec = formspec.."label["
				..tostring(2)..","..tostring(formspecy)
				..";"..F(S("This recipe is too\nlarge to be displayed.")).."]"
		end
		
		formspecy = fpy

		if craft_type.uses_crafting_grid and display_size.width <= 3 then
			formspec = formspec.."label[0,"..(formspecy + 1.9)..";" .. F(S("To craft grid:")) .. "]"
					.."button[0,  "..(formspecy + 2.5)..";0.6,0.5;craftguide_craft_1;1]"
					.."button[0.6,"..(formspecy + 2.5)..";0.7,0.5;craftguide_craft_10;10]"
					.."button[1.3,"..(formspecy + 2.5)..";0.8,0.5;craftguide_craft_max;" .. F(S("All")) .. "]"
		end
		if has_creative then
			formspec = formspec.."label[0,"..(formspecy + 3.1)..";" .. F(S("Give me:")) .. "]"
					.."button[0,  "..(formspecy + 3.7)..";0.6,0.5;craftguide_giveme_1;1]"
					.."button[0.6,"..(formspecy + 3.7)..";0.7,0.5;craftguide_giveme_10;10]"
					.."button[1.3,"..(formspecy + 3.7)..";0.8,0.5;craftguide_giveme_99;99]"
		end

		if alternates and alternates > 1 then
			formspec = formspec.."label[5.5,"..(formspecy + 2.6)..";"
					..string.format(F(recipe_text[dir]), alternate, alternates).."]"
					.."image_button[5.5,"..(formspecy + 3)..";1,1;ui_left_icon.png;alternate_prev;]"
					.."image_button[6.5,"..(formspecy + 3)..";1,1;ui_right_icon.png;alternate;]"
					.."tooltip[alternate_prev;"..F(prev_alt_text[dir]).."]"
					.."tooltip[alternate;"..F(next_alt_text[dir]).."]"
		end
		return {formspec = formspec}
	end,
})

local function craftguide_giveme(player, formname, fields)
	local amount
	for k, v in pairs(fields) do
		amount = k:match("craftguide_giveme_(.*)")
		if amount then break end
	end
	if not amount then return end

	amount = tonumber(amount)
	if amount == 0 then return end

	local player_name = player:get_player_name()

	local output = unified_inventory.current_item[player_name]
	if (not output) or (output == "") then return end

	local player_inv = player:get_inventory()

	player_inv:add_item("main", {name = output, count = amount})
end

-- tells if an item can be moved and returns an index if so
local function item_fits(player_inv, craft_item, needed_item)
	local need_group = string.sub(needed_item, 1, 6) == "group:"
	if need_group then
		need_group = string.sub(needed_item, 7)
	end
	if craft_item
	and not craft_item:is_empty() then
		local ciname = craft_item:get_name()

		-- abort if the item there isn't usable
		if ciname ~= needed_item
		and not need_group then
			return
		end

		-- abort if no item fits onto it
		if craft_item:get_count() >= craft_item:get_definition().stack_max then
			return
		end

		-- use the item there if it's in the right group and a group item is needed
		if need_group then
			if minetest.get_item_group(ciname, need_group) == 0 then
				return
			end
			needed_item = ciname
			need_group = false
		end
	end

	if need_group then
		-- search an item of the specific group
		for i,item in pairs(player_inv:get_list("main")) do
			if not item:is_empty()
			and minetest.get_item_group(item:get_name(), need_group) > 0 then
				return i
			end
		end

		-- no index found
		return
	end

	-- search an item with a the name needed_item
	for i,item in pairs(player_inv:get_list("main")) do
		if not item:is_empty()
		and item:get_name() == needed_item then
			return i
		end
	end

	-- no index found
end

-- modifies the player inventory and returns the changed craft_item if possible
local function move_item(player_inv, craft_item, needed_item)
	local stackid = item_fits(player_inv, craft_item, needed_item)
	if not stackid then
		return
	end
	local wanted_stack = player_inv:get_stack("main", stackid)
	local taken_item = wanted_stack:take_item()
	player_inv:set_stack("main", stackid, wanted_stack)

	if not craft_item
	or craft_item:is_empty() then
		return taken_item
	end

	craft_item:add_item(taken_item)
	return craft_item
end

local function craftguide_craft(player, formname, fields)
	local amount
	for k, v in pairs(fields) do
		amount = k:match("craftguide_craft_(.*)")
		if amount then break end
	end
	if not amount then return end
	local player_name = player:get_player_name()

	local output = unified_inventory.current_item[player_name]
	if (not output) or (output == "") then return end

	local player_inv = player:get_inventory()

	local crafts = unified_inventory.crafts_for[unified_inventory.current_craft_direction[player_name]][output]
	if (not crafts) or (#crafts == 0) then return end

	local alternate = unified_inventory.alternate[player_name]

	local craft = crafts[alternate]
	if craft.width > 3 then return end

	local needed = craft.items

	local craft_list = player_inv:get_list("craft")

	local width = craft.width
	if width == 0 then
		-- Shapeless recipe
		width = 3
	end

	amount = tonumber(amount) or 99
	--[[
	if amount == "max" then
		amount = 99 -- Arbitrary; need better way to do this.
	else
		amount = tonumber(amount)
	end--]]

	for iter = 1, amount do
		local index = 1
		for y = 1, 3 do
			for x = 1, width do
				local needed_item = needed[index]
				if needed_item then
					local craft_index = ((y - 1) * 3) + x
					local craft_item = craft_list[craft_index]
					local newitem = move_item(player_inv, craft_item, needed_item)
					if newitem then
						craft_list[craft_index] = newitem
					end
				end
				index = index + 1
			end
		end
	end

	player_inv:set_list("craft", craft_list)

	unified_inventory.set_inventory_formspec(player, "craft")
end

minetest.register_on_player_receive_fields(function(player, formname, fields)
	for k, v in pairs(fields) do
		if k:match("craftguide_craft_") then
			craftguide_craft(player, formname, fields)
			return
		end
		if k:match("craftguide_giveme_") then
			craftguide_giveme(player, formname, fields)
			return
		end
		if k:match("listentrybg") then
			local data = core.explode_textlist_event(v)
			if data then
				--print("verify1")
				if data.type == "CHG" and data.index then
					--print("verify2")
					local image_id = uibgidx[data.index]
					if image_id and ui_backgrounds[image_id] then
						--print("verify3")
						local meta = player:get_meta()
						if meta then
							meta:set_string("background_image", image_id)
							--core.after(0.1, function(player)
								unified_inventory.set_inventory_formspec(player,"background")
							--end, player)
						end
					end
				end
			end
		end
		if k:match("opacity") then
			local data = core.explode_scrollbar_event(v)
			if data then
				if data.type == "CHG" and data.value then
					local meta = player:get_meta()
					if meta then
						--print(data.value)
						meta:set_string("background_opacity", data.value)
						unified_inventory.set_inventory_formspec(player, "background")
					end
				end
			end
		end
	end
end)
