--!strict
-- 09_blinker.luau
-- Makes an object blink on and off forever: a timing puzzle for parkour, or just a flashing sign.
--
-- Attach it to anything and drag the object that should blink onto the Target slot.

-- External Dependencies
local Target: PhysicalComponent = nil
-- End External Dependencies

local ON_SECONDS = 2
local OFF_SECONDS = 1.5

local function cycle()
	Target:showLua()
	Target:setDefaultCollision()
	LuauClock.timeout(ON_SECONDS):andThen(function()
		Target:hideLua()
		Target:disableCollisionLua()
		return LuauClock.timeout(OFF_SECONDS)
	end):andThen(cycle)
end

function BeginPlay()
	if (Target :: PhysicalComponent?) == nil then
		log("Blinker: pick an object for the Target slot in the editor")
		return
	end
	cycle()
end
