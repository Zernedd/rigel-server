-- SuddenDeath.luau -- a timed round; a draw goes straight into a 30 second sudden-death round.
-- Comes with the example level of the same name (Levels tab); or pick it in Game Modes > Script > "Start from example".
--
-- Set up in the Game Modes window:
--   Rules:  Round length = 120, Score to win = 0
--   Pieces: Score buttons (one per team) and a Round timer
--   A Text object with the text:   {Note}

local suddenDeath = false
local suddenRound = 0

function OnRoundStart(round: number)
	if not suddenDeath then
		Rigel.setText("Note", "")
		return
	end
	suddenRound = round
	Rigel.setText("Note", "SUDDEN DEATH - next point wins")
	-- Cap it at 30 seconds, whatever the round length is. The round number tells us whether this is
	-- still the same round when the time is up.
	LuauClock.timeout(30):andThen(function()
		if Rigel.isRunning() and Rigel.round() == suddenRound then
			Rigel.endRound(0)
		end
	end)
end

function OnScore(team: number, score: number, old: number)
	if suddenDeath and Rigel.isRunning() and score > old then
		Rigel.endRound(team)             -- first point in sudden death wins
	end
end

function OnRoundEnd(winner: number)
	if winner == 0 and not suddenDeath then
		suddenDeath = true
		Rigel.startNow()                 -- no countdown: straight back in
	else
		suddenDeath = false
		Rigel.setText("Note", if winner > 0 then Rigel.teamName(winner) .. " win!" else "Still a draw!")
	end
end
