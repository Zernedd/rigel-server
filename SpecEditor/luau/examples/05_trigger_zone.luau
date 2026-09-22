--!strict
-- 05_trigger_zone.luau
-- A PhysicalComponent used as a trigger volume: greet players who walk into it,
-- and hand out a quest event.
--
-- As in 04, `Zone` comes from the object's references wiring, which the editor
-- cannot set up yet.

-- External Dependencies
local Zone: PhysicalComponent = nil
-- End External Dependencies

local visits: { [number]: number } = {}

function BeginPlay()
	if (Zone :: PhysicalComponent?) == nil then
		log("Zone reference is not wired")
		return
	end

	-- Make the volume a trigger (overlaps fire, nothing blocks).
	Zone:setTriggerCollision()

	-- Fires on the server with the index of the player who entered.
	Zone.onOverlapByPlayerServer.Listen(function(playerIndex: number)
		visits[playerIndex] = (visits[playerIndex] or 0) + 1
		local name = VRPawn.getPlayerName(playerIndex)
		local team = VRPawn.getTeamIndexByID(playerIndex)
		log(`{name} (team {team}) entered the zone, visit #{visits[playerIndex]}`)
	end)

	-- Fires on the machine of the player who entered (no argument): good for per-player
	-- effects such as quest progress. -1 means "the local player", as in the golf scripts.
	Zone.onOverlapByPlayerClientside.Listen(function()
		Quests.event(-1, "MyZone_Entered")
	end)
end
