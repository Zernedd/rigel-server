--!strict
-- 10_secret_door.luau
-- A secret door: a button opens it for a few seconds, then it closes again by itself.
--
-- Attach it to the button, then drag the button onto the Button slot and the wall/door onto the Door slot.

-- External Dependencies
local Button: BasicButtonComponent = nil
local Door: PhysicalComponent = nil
-- End External Dependencies

local OPEN_SECONDS = 5
local open = false

function BeginPlay()
	if (Button :: BasicButtonComponent?) == nil or (Door :: PhysicalComponent?) == nil then
		log("SecretDoor: pick objects for the Button and Door slots in the editor")
		return
	end
	Button.onButtonPressEvent.Listen(function()
		if open then return end
		open = true
		Door:hideLua()
		Door:disableCollisionLua()
		LuauClock.timeout(OPEN_SECONDS):andThen(function()
			Door:showLua()
			Door:setDefaultCollision()
			open = false
		end)
	end)
end
