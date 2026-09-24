--!strict
-- 04_button_toggles_door.luau
-- React to an event from a referenced component and drive another one.
--
-- IMPORTANT: the two locals in the External Dependencies block are filled in by the game from
-- the object's `references` wiring, matching by name: in the editor, Details > Game data > the
-- script's slots (click a slot, then click the object). Until wired they are nil; the nil checks
-- below keep the script from erroring in that case.

-- External Dependencies
local Button: BasicButtonComponent = nil
local Door: PhysicalComponent = nil
-- End External Dependencies

local doorOpen = false

local function setDoor(open: boolean)
	doorOpen = open
	if open then
		Door:hideLua()               -- make it invisible ...
		Door:disableCollisionLua()   -- ... and let players walk through
	else
		Door:showLua()
		Door:setDefaultCollision()
	end
	log("door " .. (if open then "opened" else "closed"))
end

function BeginPlay()
	if (Button :: BasicButtonComponent?) == nil or (Door :: PhysicalComponent?) == nil then
		log("Button/Door references are not wired; nothing to do")
		return
	end

	-- Events are listened to with a DOT: component.eventName.Listen(function(...) end)
	Button.onButtonPressEvent.Listen(function()
		setDoor(not doorOpen)
	end)

	setDoor(false)
end
