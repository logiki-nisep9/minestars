core.async_jobs = {}

function core.async_event_handler(jobid, retval)
	local callback = core.async_jobs[jobid]
	--assert(type(callback) == "function")
	--Some functions as ABMs are registered without an callback funtion, return
	if not callback then return end
	callback(unpack(retval, 1, retval.n))
	core.async_jobs[jobid] = nil
end

function core.handle_async(func, callback, ...)
	assert(type(func) == "function" and type(callback) == "function",
		"Invalid minetest.handle_async invocation")
	local args = {n = select("#", ...), ...}
	local mod_origin = core.get_last_run_mod()

	local jobid = core.do_async_callback(func, args, mod_origin)
	core.async_jobs[jobid] = callback

	return true
end

local old_set_node = core.set_node
function core.set_node(pos, node, func, ...)
	if func then
		--error("KINDERGARTEN U STUPI-")
		return old_set_node(pos, node, func, {...})
	end
	return old_set_node(pos, node)
end