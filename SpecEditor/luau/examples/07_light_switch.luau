--!strict
-- 07_light_switch.luau
-- A light switch that makes another object appear and disappear.
--
-- How to use it:
--   1. Place a Light Switch (or any switch) and the object it should control (a cube, a wall, a platform...).
--   2. Select the switch, open Details > Luau script, pick LightSwitch and click "Attach to this object".
--   3. In Details > Game data, under "Script LightSwitch.luau", drag the switch itself from the Outliner onto
--      the Switch slot, and the object onto the Target slot.
-- Flip the switch: the target shows when it's on and hides (and stops blocking players) when it's off.

-- External Dependencies (slots: drag objects onto these in the editor)
local Switch: ToggleableComponent = nil
local Target: PhysicalComponent = nil
-- End External Dependencies

local function apply(on: boolean)
	if on then
		Target:showLua()
		Target:setDefaultCollision()
	else
		Target:hideLua()
		Target:disableCollisionLua()
	end
end

function BeginPlay()
	local missing = {}
	if (Switch :: ToggleableComponent?) == nil then table.insert(missing, "Switch") end
	if (Target :: PhysicalComponent?) == nil then table.insert(missing, "Target") end
	if #missing > 0 then
		log("LightSwitch: pick objects for these slots in the editor (Details > Game data): " .. table.concat(missing, ", "))
		return
	end
	Switch.blueprintOnEnabled.Listen(function() apply(true) end)
	Switch.blueprintOnDisabled.Listen(function() apply(false) end)
	apply(Switch.isEnabled)
	log("LightSwitch ready")
end
