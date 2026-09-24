-- FirstToTen.luau -- a score race with a live leader sign.
-- Comes with the example level of the same name (Levels tab); or pick it in Game Modes > Script > "Start from example".
--
-- Set up in the Game Modes window:
--   Rules:  Score to win = 10, Round length = 0 (no time limit)
--   Pieces: a Start button, one Score button per team (role "Score button", team 1 / team 2)
--   A Text object anywhere in the mode with the text:   {Leader}
--
-- Hooks run on every machine. Rigel actions (addScore, endRound, setText ...) only count on the server,
-- so it is safe that every machine calls them.

local function leaderText(): string
	local best, bestTeam, tie = -1, 0, false
	for t = 1, Rigel.teams() do
		local s = Rigel.score(t)
		if s > best then
			best, bestTeam, tie = s, t, false
		elseif s == best then
			tie = true
		end
	end
	if best <= 0 then
		return "First to " .. Rigel.settingNumber("score_to_win") .. "!"
	elseif tie then
		return "Tied on " .. best
	end
	return Rigel.teamName(bestTeam) .. " lead with " .. best
end

function OnLobby()
	Rigel.setText("Leader", "Press the start button")
end

function OnRoundStart(round: number)
	Rigel.setText("Leader", leaderText())
end

function OnScore(team: number, score: number, old: number)
	Rigel.setText("Leader", leaderText())
end

function OnRoundEnd(winner: number)
	if winner > 0 then
		Rigel.setText("Leader", Rigel.teamName(winner) .. " win!")
	else
		Rigel.setText("Leader", "Draw!")
	end
end
