-- BallBattle.luau -- a Jake ball match: first to 5, the ball goes back to the middle after every point.
-- Comes with the example level of the same name (Levels tab); or pick it in Game Modes > Script > "Start from example".
--
-- Set up in the Game Modes window:
--   Pieces: a Jake ball spawner in the middle (role "Ball spawner"      -> Ball1)
--           one Score button per team        (role "Score button", team 1 / team 2)
--           a Scoreboard monitor and a Score board (role "Scoreboard") so everyone sees the score
--   Rules:  Start = Start button, Score to win = 5, Round length = 300
--
-- Balls are the SERVER's job: it spawns (or resets) every "Ball spawner" ball when a round starts, and
-- Rigel.resetBalls() does it again whenever you ask. Never call spawnBall from a game mode script -- the
-- script runs on every machine and each would make its own ball that nobody else can see.

function OnRoundStart(round: number)
	Rigel.setText("Match", "Round " .. round .. " -- first to " .. Rigel.settingNumber("score_to_win"))
end

function OnScore(team: number, score: number, old: number)
	if score > old and Rigel.isRunning() then
		Rigel.setText("Match", Rigel.teamName(team) .. " scored! " .. Rigel.score(1) .. " - " .. Rigel.score(2))
		Rigel.resetBalls()               -- back to the middle
	end
end

function OnRoundEnd(winner: number)
	Rigel.setText("Match", if winner > 0 then Rigel.teamName(winner) .. " win the match!" else "Draw!")
	-- Ball1 is this mode's ball spawner; freeze its ball until the next round.
	pcall(function()
		local ball = Ball1:getSpawnedBall()
		if ball then
			ball:freeze(true)
		end
	end)
end
