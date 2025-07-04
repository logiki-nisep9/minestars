after_time = 0.0
after_time_next = math.huge
after_jobs = {}
--[[

core.register_globalstep(function(dtime)
	time = time + dtime

	if time < time_next then
		return
	end

	time_next = math.huge

	-- Iterate backwards so that we miss any new timers added by
	-- a timer callback.
	for i = #jobs, 1, -1 do
		local job = jobs[i]
		if time >= job.expire then
			core.set_last_run_mod(job.mod_origin)
			job.func(unpack(job.arg))
			local jobs_l = #jobs
			jobs[i] = jobs[jobs_l]
			jobs[jobs_l] = nil
		elseif job.expire < time_next then
			time_next = job.expire
		end
	end
end)--]]

function core.after(after, func, ...)
	assert(tonumber(after) and type(func) == "function",
		"Invalid minetest.after invocation")
	local expire = after_time + after
	local new_job = {
		func = func,
		expire = expire,
		arg = {...},
		mod_origin = core.get_last_run_mod(),
	}
	after_jobs[#after_jobs + 1] = new_job
	after_time_next = math.min(after_time_next, expire)
	return { cancel = function() new_job.func = function() end end }
end
