--!strict
-- 08_vanishing_platform.luau
-- A platform that disappears a moment after a player touches it, then comes back. Great for parkour.
--
-- Attach it to the platform (a cube or any mesh) and drag that same object onto the Platform slot.

-- External Dependencies
local Platform: PhysicalComponent = nil
-- End External Dependencies

local VANISH_AFTER = 0.6   -- seconds after the first touch
local COME_BACK_AFTER = 3  -- seconds hidden

local busy = false

function BeginPlay()
	if (Platform :: PhysicalComponent?) == nil then
		log("VanishingPlatform: pick the platform for the Platform slot in the editor")
		return
	end
	Platform.onOverlapByPlayerSimple.Listen(function()
		if busy then return end
		busy = true
		LuauClock.timeout(VANISH_AFTER):andThen(function()
			Platform:hideLua()
			Platform:disableCollisionLua()
			return LuauClock.timeout(COME_BACK_AFTER)
		end):andThen(function()
			Platform:showLua()
			Platform:setDefaultCollision()
			busy = false
		end)
	end)
end
