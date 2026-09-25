-- MiniJakeball: the station's Jakeball flow (TKB gamemode.luau), ported state for state onto this arena.
--   * between games the ball drops out of the ring; carry it back up INTO the ring to start
--   * kickoff: the ball glides back to its spot above the ring and freezes (3 s), dips 90 cm into the ring (1 s),
--     a 15 s countdown, then it is flung up into play (35 m/s, within 15 degrees of straight up)
--   * a goal: 4 s celebration, the ball is released, a 15 s countdown -- ended early when the ball is carried over
--     a midfield trigger; the goals are only live while play is running
--   * the clock runs out: a tie goes to golden-goal overtime; otherwise play waits for the ball to land, then ends
--   * mercy rule, rounds per match, the arena state / clock / scores on the monitors, as in the station
-- Only the server runs it (the only machine that has the mode's ball); every other copy stays idle.
-- The station's quests / stats, team debuff volumes, practice mode and control panel are not part of this arena.

local CX, CY, CZ = @CX@, @CY@, @CZ@
local U = { @U@ }
local V = { @V@ }
local UPV = { @UP@ }
local HALF_L, HALF_W, TOP = @HL@, @HW@, @TOP@

local MATCH_SECONDS = 300
local ROUNDS_PER_MATCH = 1
local USE_BEST_OF = false
local TIME_BETWEEN_ROUNDS = 10
local MERCY_SCORE_DIFFERENCE = 5
local COUNTDOWN_MS = 15000
local FLING_SPEED = 3500

local State = {
	NOT_RUNNING = 0,
	WAITING_TO_START = 1,
	GAME_BEGIN_ENTER = 2,
	GAME_BEGIN = 3,
	GAME_START_COUNTDOWN = 4,
	RUNNING = 5,
	TEAM_SCORED_ENTER = 6,
	TEAM_SCORED = 7,
	NEXT_POINT_COUNTDOWN = 8,
	TIMER_ENDED = 9,
	GAME_EXIT = 10,
	OVERTIME_ENTER = 11,
	OVERTIME_COUNTDOWN = 12,
	OVERTIME_RUNNING = 13,
	BEGIN_PLAY = 14,
}

local currentState = State.NOT_RUNNING
local team0points = 0
local team1points = 0
local team0RoundsWon = 0
local team1RoundsWon = 0
local bMoreRoundsToPlay = false
local bGameWasRestarted = false
local waitForBallGrounded = false
local isBallGrounded = false
local basicToken = 0
local booted = false

-- ---------------------------------------------------------------------------------------------------- helpers
local function ball()
	local b = nil
	pcall(function()
		b = Ball1:getSpawnedBall()
	end)
	return b
end

local function vec(a, s: number)
	return Vector.new(a[1] * s, a[2] * s, a[3] * s)
end

local function setFrozen(on: boolean)
	local b = ball()
	if b == nil then
		return
	end
	pcall(function()
		if on then
			b:freeze(true)
		else
			b:unFreeze(true)
		end
	end)
end

local function celebrate(team: number, on: boolean)
	pcall(function()
		local c = if team == 0 then Celebrate1 else Celebrate2
		if on then
			c:luaEnable()
		else
			c:luaDisable()
		end
	end)
end

local function goalEffects(on: boolean)
	pcall(function()
		Goal1:enableGoal(on)
	end)
	pcall(function()
		Goal2:enableGoal(on)
	end)
end

local function showScores()
	pcall(function()
		Rigel.setScore(1, team0points)
		Rigel.setScore(2, team1points)
	end)
	pcall(function()
		Gamemode:setTeamScore(0, team0points)
		Gamemode:setTeamScore(1, team1points)
	end)
end

local function along(p: Vector, a): number
	return (p.x - CX) * a[1] + (p.y - CY) * a[2] + (p.z - CZ) * a[3]
end

-- ------------------------------------------------------------------------------------------------ the flow
local SwitchState

local function SetupForPlay()
	team0points = 0
	team1points = 0
	showScores()
	pcall(function()
		ModeTimer:resetTimer()
		ModeTimer:setRoundTime(MATCH_SECONDS * 1000)
	end)
	setFrozen(false)
end

local function HandleGameEndLogic()
	if team0points > team1points then
		team0RoundsWon += 1
	else
		team1RoundsWon += 1
	end
	local teamWhoLostMatch = -1
	if USE_BEST_OF then
		if team0RoundsWon > ROUNDS_PER_MATCH / 2 then
			teamWhoLostMatch = 1
		elseif team1RoundsWon > ROUNDS_PER_MATCH / 2 then
			teamWhoLostMatch = 0
		elseif team0RoundsWon + team1RoundsWon == ROUNDS_PER_MATCH then
			teamWhoLostMatch = -1
		end
		if teamWhoLostMatch ~= -1 or team0RoundsWon + team1RoundsWon == ROUNDS_PER_MATCH then
			team0RoundsWon = 0
			team1RoundsWon = 0
		end
	else
		if team0RoundsWon + team1RoundsWon == ROUNDS_PER_MATCH then
			if team0RoundsWon > team1RoundsWon then
				teamWhoLostMatch = 1
			elseif team1RoundsWon > team0RoundsWon then
				teamWhoLostMatch = 0
			end
			team0RoundsWon = 0
			team1RoundsWon = 0
		end
	end
	bMoreRoundsToPlay = teamWhoLostMatch == -1 and ROUNDS_PER_MATCH > 1
	pcall(function()
		Gamemode:setTeamRoundsWon(0, team0RoundsWon)
		Gamemode:setTeamRoundsWon(1, team1RoundsWon)
	end)
end

local onEnter = {}
onEnter[State.BEGIN_PLAY] = {
	enter = function()
		SetupForPlay()
	end,
	nextState = State.WAITING_TO_START,
	basic = 5,
}
onEnter[State.WAITING_TO_START] = {
	enter = function()
		celebrate(0, false)
		celebrate(1, false)
		pcall(function()
			ModeTimer:luaSetGameBegun(false)
		end)
		if bMoreRoundsToPlay and not bGameWasRestarted then
			pcall(function()
				ModeTimer:startCountdown(TIME_BETWEEN_ROUNDS * 1000)
			end)
		else
			pcall(function()
				StartRing1:showLua()
				StartRing1:setTriggerCollision()
			end)
		end
		setFrozen(false)
	end,
}
onEnter[State.GAME_BEGIN_ENTER] = {
	enter = function()
		pcall(function()
			Ball1:smoothResetBall()
		end)
		setFrozen(true)
		pcall(function()
			Gamemode:startGame()
		end)
		pcall(function()
			Rigel.startNow()
		end)
		pcall(function()
			ModeTimer:resetTimer()
			ModeTimer:setRoundTime(MATCH_SECONDS * 1000)
			ModeTimer:setClockBounce(true)
		end)
		bGameWasRestarted = false
	end,
	nextState = State.GAME_BEGIN,
	basic = 3,
}
onEnter[State.GAME_BEGIN] = {
	enter = function()
		pcall(function()
			Ball1:addSmoothOffset(vec(UPV, -90))
		end)
	end,
	nextState = State.GAME_START_COUNTDOWN,
	basic = 1,
}
onEnter[State.GAME_START_COUNTDOWN] = {
	enter = function()
		pcall(function()
			ModeScore:resetScore()
		end)
		pcall(function()
			ModeTimer:startTimerWithCountdown(COUNTDOWN_MS)
		end)
	end,
}
onEnter[State.RUNNING] = {
	enter = function()
		celebrate(0, false)
		celebrate(1, false)
		pcall(function()
			ModeTimer:luaSetGameBegun(true)
		end)
		goalEffects(true)
	end,
}
onEnter[State.TEAM_SCORED_ENTER] = {
	enter = function()
		goalEffects(false)
	end,
	nextState = State.TEAM_SCORED,
	basic = 4,
}
onEnter[State.TEAM_SCORED] = {
	enter = function()
		setFrozen(false)
	end,
	nextState = State.NEXT_POINT_COUNTDOWN,
	basic = 1,
}
onEnter[State.NEXT_POINT_COUNTDOWN] = {
	enter = function()
		pcall(function()
			ModeTimer:startCountdown(COUNTDOWN_MS)
		end)
	end,
}
onEnter[State.TIMER_ENDED] = {
	enter = function() end,
	nextState = State.GAME_EXIT,
	basic = 3,
}
onEnter[State.OVERTIME_ENTER] = {
	enter = function()
		goalEffects(false)
	end,
	nextState = State.OVERTIME_COUNTDOWN,
	basic = 4,
}
onEnter[State.OVERTIME_COUNTDOWN] = {
	enter = function()
		setFrozen(false)
		pcall(function()
			ModeTimer:startCountdown(COUNTDOWN_MS)
		end)
	end,
}
onEnter[State.OVERTIME_RUNNING] = {
	enter = function()
		celebrate(0, false)
		celebrate(1, false)
		goalEffects(true)
		setFrozen(false)
	end,
}
onEnter[State.GAME_EXIT] = {
	enter = function()
		celebrate(0, false)
		celebrate(1, false)
		setFrozen(false)
		isBallGrounded = false
		pcall(function()
			Ball1:stopListeningForBallGrounded()
		end)
		pcall(function()
			ModeTimer:luaSetGameBegun(false)
		end)
		pcall(function()
			Gamemode:stopGame()
		end)
		goalEffects(false)
		pcall(function()
			ModeTimer:pauseTimer()
		end)
		if not bGameWasRestarted then
			HandleGameEndLogic()
		end
		local winner = if team0points > team1points then 1 elseif team1points > team0points then 2 else 0
		pcall(function()
			Rigel.endRound(winner)
		end)
	end,
	nextState = State.BEGIN_PLAY,
	basic = 2,
}

local StateNames = {}
for name, n in State do
	StateNames[n] = name
end

SwitchState = function(state: number)
	log(`MiniJakeball: {StateNames[state] or tostring(state)}`)
	pcall(function()
		ModeState:updateGameState(state)
	end)
	currentState = state
	local e = onEnter[state]
	if e == nil then
		return
	end
	e.enter()
	if e.basic ~= nil then
		basicToken += 1
		local token = basicToken
		LuauClock.timeout(e.basic):andThen(function()
			if token == basicToken then
				local now = onEnter[currentState]
				if now ~= nil and now.nextState ~= nil then
					SwitchState(now.nextState)
				end
			end
		end)
	end
end

local countdownFinishedDirections = {}
countdownFinishedDirections[State.GAME_START_COUNTDOWN] = State.RUNNING
countdownFinishedDirections[State.NEXT_POINT_COUNTDOWN] = State.RUNNING
countdownFinishedDirections[State.WAITING_TO_START] = State.GAME_BEGIN_ENTER
countdownFinishedDirections[State.OVERTIME_COUNTDOWN] = State.OVERTIME_RUNNING

local function OnTimerCountdownFinished()
	if currentState == State.GAME_START_COUNTDOWN then
		setFrozen(false)
		-- flung up out of the ring, within 15 degrees of straight up (the station's numbers)
		local randomX = math.random(0, 2 * math.pi)
		local randomY = math.random(0, 2 * math.pi)
		local tilt = math.tan(math.rad(15))
		local sideU, sideV = math.cos(randomX) * tilt, math.sin(randomY) * tilt
		local dir = {
			UPV[1] + U[1] * sideU + V[1] * sideV,
			UPV[2] + U[2] * sideU + V[2] * sideV,
			UPV[3] + U[3] * sideU + V[3] * sideV,
		}
		local b = ball()
		if b ~= nil then
			pcall(function()
				b:setVelocity(vec(dir, FLING_SPEED))
			end)
		end
		pcall(function()
			StartRing1:hideLua()
			StartRing1:disableCollisionLua()
		end)
	end
	local nextState = countdownFinishedDirections[currentState]
	if nextState ~= nil then
		SwitchState(nextState)
	end
end

local function OnTimerFinished()
	if
		(
			currentState == State.RUNNING
			or currentState == State.TEAM_SCORED
			or currentState == State.TEAM_SCORED_ENTER
			or currentState == State.NEXT_POINT_COUNTDOWN
		) and not bGameWasRestarted
	then
		if team0points == team1points then
			if currentState == State.TEAM_SCORED or currentState == State.TEAM_SCORED_ENTER then
				SwitchState(State.OVERTIME_COUNTDOWN)
				return
			end
			SwitchState(State.OVERTIME_RUNNING)
			return
		end
		if not isBallGrounded then
			-- keep the clock at 0 while the last ball is in the air
			pcall(function()
				ModeTimer:setRoundTime(0)
			end)
			waitForBallGrounded = true
			pcall(function()
				Ball1:beginListeningForBallGrounded()
			end)
			return
		end
	end
	if currentState == State.OVERTIME_RUNNING and team0points == team1points then
		return
	end
	pcall(function()
		ModeTimer:pauseTimer()
	end)
	SwitchState(State.GAME_EXIT)
end

local function BallNowGrounded()
	isBallGrounded = true
	pcall(function()
		Ball1:stopListeningForBallGrounded()
	end)
	if waitForBallGrounded then
		waitForBallGrounded = false
		OnTimerFinished()
	end
end

local function OnTeamScored(goalInfo: any, team: number?)
	-- team = the goal's scoring team (1 or 2) from the wiring (Goal1_team, ...): the game's goalInfo.scoringTeam reads
	-- the same for both of this arena's goals, so it's only the fallback. teamNum is 0-based, as in the station script.
	local teamNum = if team ~= nil then team - 1 else goalInfo.scoringTeam
	log(`MiniJakeball: goal for team {tostring(teamNum)} ({tostring(goalInfo.goalPoints)} pt) in {StateNames[currentState] or "?"}`)
	local points = goalInfo.goalPoints or 1
	if teamNum == 0 then
		team0points += points
		celebrate(0, true)
	elseif teamNum == 1 then
		team1points += points
		celebrate(1, true)
	end
	showScores()
	-- the clock only "bounces" while the scores are level
	pcall(function()
		ModeTimer:setClockBounce(team0points == team1points)
	end)
	if currentState == State.RUNNING then
		local b = ball()
		if b ~= nil then
			pcall(function()
				b:freeze(true)
				b:setVelocity(Vector.new(0, 0, 0))
			end)
		end
		if team0points >= team1points + MERCY_SCORE_DIFFERENCE or team1points >= team0points + MERCY_SCORE_DIFFERENCE then
			SwitchState(State.GAME_EXIT)
			return
		end
		if waitForBallGrounded then
			pcall(function()
				Ball1:stopListeningForBallGrounded()
			end)
			waitForBallGrounded = false
			isBallGrounded = false
			if team0points == team1points then
				SwitchState(State.OVERTIME_ENTER)
			else
				SwitchState(State.GAME_EXIT)
			end
			return
		end
		SwitchState(State.TEAM_SCORED_ENTER)
	elseif currentState == State.OVERTIME_RUNNING then
		setFrozen(true)
		pcall(function()
			ModeTimer:pauseTimer()
		end)
		SwitchState(State.GAME_EXIT)
	end
end

local function OnBallHitsStartTrigger(disc: any)
	if disc ~= ball() then
		return
	end
	if currentState == State.WAITING_TO_START then
		SwitchState(State.GAME_BEGIN_ENTER)
	end
end

local function OnBallHitsMidfieldTrigger(disc: any)
	if disc ~= ball() then
		return
	end
	if currentState == State.NEXT_POINT_COUNTDOWN or currentState == State.OVERTIME_COUNTDOWN then
		log(`MiniJakeball: ball over midfield, countdown skipped`)
		pcall(function()
			ModeTimer:skipCountdown()
		end)
	end
end

-- ------------------------------------------------------------------------------------------------ start up
-- On the server the mode's ball exists; wire everything once, then begin as the station arena does.
LuauClock.createTimer(1, function()
	if booted then
		-- the ball left the arena (the doorway is the only way out): back to its spawner
		if currentState == State.RUNNING or currentState == State.OVERTIME_RUNNING then
			local b = ball()
			if b ~= nil then
				local ok, p = pcall(function()
					return b:getDiscPosition()
				end)
				if ok and p ~= nil then
					local h = along(p, UPV)
					if math.abs(along(p, U)) > HALF_L or math.abs(along(p, V)) > HALF_W or h > TOP or h < -200 then
						Rigel.resetBalls()
					end
				end
			end
		end
		return
	end
	if ball() == nil then
		return
	end
	booted = true
	pcall(function()
		ModeTimer.onCountdownEnd.Listen(OnTimerCountdownFinished)
	end)
	pcall(function()
		ModeTimer.onTimerStopped.Listen(OnTimerFinished)
	end)
	pcall(function()
		Ball1.onBallGrounded.Listen(BallNowGrounded)
	end)
	pcall(function()
		Goal1.onGoalScored.Listen(function(info: any)
			OnTeamScored(info, Goal1_team)
		end)
	end)
	pcall(function()
		Goal2.onGoalScored.Listen(function(info: any)
			OnTeamScored(info, Goal2_team)
		end)
	end)
	pcall(function()
		StartRing1.onOverlapByDisc.Listen(OnBallHitsStartTrigger)
	end)
	pcall(function()
		Midfield1.onTwoWayOverlapByDisc.Listen(OnBallHitsMidfieldTrigger)
	end)
	pcall(function()
		Midfield2.onTwoWayOverlapByDisc.Listen(OnBallHitsMidfieldTrigger)
	end)
	goalEffects(false)
	celebrate(0, false)
	celebrate(1, false)
	SwitchState(State.BEGIN_PLAY)
end)

-- The server ended the game itself (everyone left their team): exit like a restart, no rounds counted.
function OnRoundEnd(winner: number)
	if not booted then
		return
	end
	if
		currentState ~= State.GAME_EXIT
		and currentState ~= State.BEGIN_PLAY
		and currentState ~= State.WAITING_TO_START
		and currentState ~= State.NOT_RUNNING
	then
		bGameWasRestarted = true
		SwitchState(State.GAME_EXIT)
	end
end
