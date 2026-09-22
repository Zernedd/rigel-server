--!strict
-- 02_repeating_timer.luau
-- A timer loop: log a heartbeat every 2 seconds, and stop after 5 beats.
--
-- LuauClock.timeout(seconds) returns a promise that resolves after that many seconds.
-- :andThen(fn) runs fn when it resolves. To repeat, schedule the next timeout from inside fn.

local INTERVAL = 2      -- seconds between beats
local MAX_BEATS = 5

local beats = 0
local startedAt = 0

local function beat()
	beats += 1
	local elapsed = LuauClock.getTime() - startedAt
	log(`beat {beats} at {math.floor(elapsed * 10 + 0.5) / 10} s`)

	if beats < MAX_BEATS then
		LuauClock.timeout(INTERVAL):andThen(beat)
	else
		log("done")
	end
end

function BeginPlay()
	startedAt = LuauClock.getTime()
	LuauClock.timeout(INTERVAL):andThen(beat)
end
