-- TrapGauntlet.luau -- traps that get meaner as the round runs out.
-- Comes with the example level of the same name (Levels tab); or pick it in Game Modes > Script > "Start from example".
--
-- Set up in the Game Modes window (the slot names come from the roles -- the window shows them):
--   Pieces: some traps with the role "Trap - fired by trap buttons"   -> TrapFired1, TrapFired2, ...
--           a spinner (or any trap) with "Trap - on during rounds"     -> TrapRound1
--           a Round timer                                               -> Timer1
--   Rules:  Round length = 90
--
-- For the last 20 seconds every "fired" trap stays on. Slots that don't exist are simply nil,
-- so each use is checked.

local finale = false

local function setFired(on: boolean)
	for _, trap in { TrapFired1, TrapFired2, TrapFired3, TrapFired4 } do
		pcall(function()
			if on then trap:luaEnable() else trap:luaDisable() end
		end)
	end
end

function OnRoundStart(round: number)
	finale = false
end

function OnTime(secondsLeft: number)
	if Rigel.isRunning() and not finale and secondsLeft > 0 and secondsLeft <= 20 then
		finale = true
		setFired(true)
		Rigel.setText("Warning", "FINAL 20 SECONDS")
	end
end

function OnRoundEnd(winner: number)
	finale = false
	setFired(false)
	Rigel.setText("Warning", "")
end
