--!strict
-- 03_promise_sequence.luau
-- Two ways to wait: chaining with :andThen, and writing straight-line code with :await().
-- Both patterns are taken from the game's own scripts (TKBGolf, ScraprunPrime).

-- Straight-line style: inside Promise.new(function() ... end) you may call :await(),
-- which pauses only this function, not the game.
local function countdown(from: number)
	Promise.new(function()
		for n = from, 1, -1 do
			log("T-minus " .. n)
			LuauClock.timeout(1):await()
		end
		log("Go!")
	end)
end

-- Racing a timeout against something that may happen first (TKBGolf uses this to hide an
-- arrow after 45 seconds OR as soon as the player starts another hole).
local cancel = (function() end) :: () -> ()   -- a cast: a typed top-level local would be an editor property

local function waitForSignalOrTimeout(seconds: number)
	local signalled = Promise.new(function(resolve: (number) -> (), _reject: (any) -> ())
		cancel = function()
			resolve(1)
		end
	end)

	Promise.race({ LuauClock.timeout(seconds), signalled }):andThen(function()
		cancel = function() end
		log("either the timeout fired or cancel() was called")
	end)
end

function BeginPlay()
	countdown(3)
	waitForSignalOrTimeout(10)
	-- Something else in your script could call cancel() to finish early:
	LuauClock.timeout(4):andThen(function()
		cancel()
	end)
end
