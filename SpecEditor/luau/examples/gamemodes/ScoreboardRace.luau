-- ScoreboardRace.luau -- a button-mashing race that shows every kind of scoreboard.
-- Comes with the example level of the same name (Levels tab); or pick it in Game Modes > Script > "Start from example".
--
-- Scoreboards need no code: the mode's controller script keeps them up to date by itself.
--   Scoreboard monitor / side scoreboard  -> the mode's team scores and rounds won (just place them)
--   Score board (role "Scoreboard")        -> team 1 / team 2 points
--   Score table (role "Score table")       -> one row per team: name, score, players, rounds won
--   Text objects with {ScoreboardRace.team1.score} etc. -> anything else you like
--
-- This code only adds a bonus: the team that is behind gets 2 points per press in the last 15 seconds.

local catchUp = false

function OnRoundStart(round: number)
	catchUp = false
end

function OnTime(secondsLeft: number)
	if Rigel.isRunning() and secondsLeft > 0 and secondsLeft <= 15 and not catchUp then
		catchUp = true
		Rigel.setText("Race", "CATCH-UP: the team behind scores double!")
	end
end

function OnScore(team: number, score: number, old: number)
	if not catchUp or score <= old then
		return
	end
	local other = if team == 1 then 2 else 1
	-- a normal press gives +1; a team that was behind before this press gets one extra point
	if old < Rigel.score(other) and score - old == 1 then
		Rigel.addScore(team, 1)
	end
end

function OnRoundEnd(winner: number)
	catchUp = false
	Rigel.setText("Race", if winner > 0 then Rigel.teamName(winner) .. " win!" else "Draw!")
end
