-- External Definitions, Please do not touch
-- =========================================================================
local CenterDiscCylinder: PhysicalComponent = nil
local primaryBall: DiscEntity = nil
local primarySpawner: BallSpawnerComponent = nil

local GameScore: ScoreComponent = nil

local GameTimer: GameTimeComponent = nil

local MidfieldDiscPhysical: PhysicalComponent = nil
local MidfieldToggleable: ToggleableComponent = nil

local MidfieldDiscPhysical2: PhysicalComponent = nil
local MidfieldToggleable2: ToggleableComponent = nil

local RedGoalToggleable: ToggleableComponent = nil
local BlueGoalToggleable: ToggleableComponent = nil

local RedGoal: GoalComponent = nil
local BlueGoal: GoalComponent = nil

local DriftballControlPanelComp: DriftballControlPanelComponent = nil
local DriftballControlPanelBlueprint: DriftballControlPanelBlueprintComponent = nil
local DriftballControlPanelToggle: ToggleableComponent = nil

local GameStateManager: GameStateManagerComponent = nil

local GameStateManagerBP: GameStateManagerBlueprintComponent = nil

local BlueCooldownVolume: ToggleableComponent = nil
local RedCooldownVolume: ToggleableComponent = nil

local HomeTeamKickOutVolume: ToggleableComponent = nil
local AwayTeamKickOutVolume: ToggleableComponent = nil

local BasicTimer: TimerComponent = nil

local BoostBuffToggleable: ToggleableComponent = nil

local RedTeamScoredCelebration: ToggleableComponent = nil
local BlueTeamScoredCelebration: ToggleableComponent = nil


function RemoveDebuffs()
	--MidfieldDiscPhysical:hideLua()
	MidfieldToggleable:luaDisable()
	--MidfieldDiscPhysical2:hideLua()
	MidfieldToggleable2:luaDisable()

	RedCooldownVolume:luaDisable()
	BlueCooldownVolume:luaDisable()
end

function ShowDebuffVolume()
	MidfieldDiscPhysical:showLua()
	MidfieldToggleable:luaEnable()
end

function ShowDebuffVolume2()
	MidfieldDiscPhysical2:showLua()
	MidfieldToggleable2:luaEnable()
end

function EnableGoalEffects()
	RedGoalToggleable:luaEnable()
	BlueGoalToggleable:luaEnable()
end

function DisableGoalEffects()
 	RedGoalToggleable:luaDisable()
	BlueGoalToggleable:luaDisable()
end



function ApplyTeam0Debuff()
	BlueCooldownVolume:luaEnable()
end
function ApplyTeam1Debuff()
	RedCooldownVolume:luaEnable()
end

isBallGrounded = false
waitForBallGrounded = false
team0points = 0
team1points = 0

team0RoundsWon = 0
team1RoundsWon = 0

bGameWasRestarted = false
bPracticeModeEnabled = false
bRunningGamemodeTests = false

bUseBestOf = false
RoundsToPlay = 1


-- =========================================================================

-- currentState is the currentState of the gamemode
State = {
	NOT_RUNNING = 0, -- Game is not running

	WAITING_TO_START = 1, -- Server just started up, Wait 20 seconds before starting the game

	GAME_BEGIN_ENTER = 2, -- Game is waiting to start

	GAME_BEGIN = 3,

	GAME_START_COUNTDOWN = 4,

	RUNNING = 5, -- Running

	TEAM_SCORED_ENTER = 6, -- Team has scored, Wait

	TEAM_SCORED = 7, -- Team has scored, Wait

	NEXT_POINT_COUNTDOWN = 8, -- Game is waiting to start

	TIMER_ENDED = 9, -- Game is over, Wait

	GAME_EXIT = 10, -- Game is over, Wait

	OVERTIME_ENTER = 11,
	OVERTIME_COUNTDOWN = 12,
	OVERTIME_RUNNING = 13,
	BEGIN_PLAY = 14, -- Server just started up, Wait 20 seconds before starting the game

	PRACTICE_START = 15,
	PRACTICE_ON_SCORED = 16,
	PRACTICE_FLING_BALL = 17,
	PRACTICE_RUNNING = 18,
	PRACTICE_ON_PASS_CAUGHT = 19,

	-- OVERTIME_PREP_TIME = 11
}

StateNames = {
	[State.NOT_RUNNING] = "NOT_RUNNING",
	[State.WAITING_TO_START] = "WAITING_TO_START",
	[State.GAME_BEGIN_ENTER] = "GAME_BEGIN_ENTER",
	[State.GAME_BEGIN] = "GAME_BEGIN",
	[State.GAME_START_COUNTDOWN] = "GAME_START_COUNTDOWN",
	[State.RUNNING] = "RUNNING",
	[State.TEAM_SCORED_ENTER] = "TEAM_SCORED_ENTER",
	[State.TEAM_SCORED] = "TEAM_SCORED",
	[State.NEXT_POINT_COUNTDOWN] = "NEXT_POINT_COUNTDOWN",
	[State.TIMER_ENDED] = "TIMER_ENDED",
	[State.GAME_EXIT] = "GAME_EXIT",
	[State.OVERTIME_ENTER] = "OVERTIME_ENTER",
	[State.OVERTIME_COUNTDOWN] = "OVERTIME_COUNTDOWN",
	[State.OVERTIME_RUNNING] = "OVERTIME_RUNNING",
	[State.BEGIN_PLAY] = "BEGIN_PLAY",
	[State.PRACTICE_START] = "PRACTICE_START",
	[State.PRACTICE_ON_SCORED] = "PRACTICE_ON_SCORED",
	[State.PRACTICE_FLING_BALL] = "PRACTICE_FLING_BALL",
	[State.PRACTICE_RUNNING] = "PRACTICE_RUNNING",
	[State.PRACTICE_ON_PASS_CAUGHT] = "PRACTICE_ON_PASS_CAUGHT",
}

currentState = State.NOT_RUNNING

MAX_OVERTIME_COUNT = 2
currentOvertimeCount = 0

hasShotDiscWaitingForCountdown = false

processingRecentHit = false

bMoreRoundsToPlay = false



function OnTimerFinished(): ()
	-- ----logic for "keep the ball up" functionality
	-- make sure we handle all states of the game when the timer could end
	if (currentState == State.RUNNING or currentState == State.TEAM_SCORED 
	or currentState == State.TEAM_SCORED_ENTER or currentState == State.NEXT_POINT_COUNTDOWN) and bGameWasRestarted == false 
	then
		if team0points == team1points then
			if (currentState == State.TEAM_SCORED or currentState == State.TEAM_SCORED_ENTER) then
				SwitchState(State.OVERTIME_COUNTDOWN)
				return
			end
			SwitchState(State.OVERTIME_RUNNING)
			return
		end


		if not isBallGrounded then
			-- this is to keep the timer at 0 during this wait
			GameTimer:setRoundTime(0)
			waitForBallGrounded = true
			primarySpawner:beginListeningForBallGrounded()
			return
		end
    end

    if ((currentState == State.OVERTIME_RUNNING) and (team0points == team1points)) then
        return
	end
	--
	GameTimer:pauseTimer()
	----log("DisableGoalEffects OnTimerFinished, about to exit")
	DisableGoalEffects()
	SwitchState(State.GAME_EXIT)
end



countdownFinishedDirections = {
	[State.GAME_START_COUNTDOWN] = State.RUNNING,
	[State.NEXT_POINT_COUNTDOWN] = State.RUNNING,
	[State.WAITING_TO_START] = State.GAME_BEGIN_ENTER,
	[State.OVERTIME_COUNTDOWN] = State.OVERTIME_RUNNING,
}

----
-- function Debug_SetTimer10Minutes(): ()
-- 	Invoke_SetMaxTime(600000)
-- end
-- function Debug_SetTimer1Minutes(): ()
-- 	Invoke_SetMaxTime(60000)
-- end
-- function Debug_SetTimer15Seconds(): ()
-- 	Invoke_SetMaxTime(15000)
-- end

function Debug_OnGameStart(): ()
	OnGameStart()
end
function OnGameStart()
	SwitchState(State.GAME_START_COUNTDOWN)


end

function OnBallHitsStartTrigger(disc: DiscEntity)
	if (primarySpawner and disc ~= primarySpawner:getSpawnedBall()) then
		return
	end
	if currentState == State.WAITING_TO_START then
		SwitchState(State.GAME_BEGIN_ENTER)
	end
end



function OnForceBallHitsStartTrigger(disc: DiscEntity)
	if currentState == State.WAITING_TO_START then
		bRunningGamemodeTests = true
		SwitchState(State.GAME_BEGIN_ENTER)
	end
end

function SetupForPlay()
	if primarySpawner == nil then
		------log("spawner is nil")
	else
		primaryBall = primarySpawner:getSpawnedBall()
		if primaryBall == nil then
			------log("now ball is nil")
		end
	end
	if (not bMoreRoundsToPlay) then
		DriftballControlPanelBlueprint:setIsRestartable(false)
	end
	team0points = 0
	team1points = 0
	GameTimer:resetTimer()
	GameTimer:setRoundTime(Gamemode:getNumberConfigVariable("MatchLengthSeconds") * 1000)

	if primaryBall ~= nil then
		primaryBall:unFreeze(true)
	end
end

stateMachineOnEnters = {
	[State.NOT_RUNNING] = nil,
	[State.RUNNING] = {
        onEnter = function()
			BlueTeamScoredCelebration:luaDisable()
			RedTeamScoredCelebration:luaDisable()
            GameTimer:luaSetGameBegun(true)
			BoostBuffToggleable:luaDisable()
            EnableGoalEffects()
            RemoveDebuffs()
        end,
    },
    [State.BEGIN_PLAY] = {
        onEnter = function() 
            SetupForPlay()
        end,
        nextState = State.WAITING_TO_START,
        basicTimerLength = 5
    },
	[State.WAITING_TO_START] = {
		onEnter = function()

			if (bRunningGamemodeTests) then
				bRunningGamemodeTests = false
			end

			BlueTeamScoredCelebration:luaDisable()
			RedTeamScoredCelebration:luaDisable()
			BoostBuffToggleable:luaDisable()
			AwayTeamKickOutVolume:luaDisable()
			HomeTeamKickOutVolume:luaDisable()
			if primarySpawner == nil then
				------log("spawner is nil")
			else
				primaryBall = primarySpawner:getSpawnedBall()
				if primaryBall == nil then
					------log("now ball is nil")
				end
			end
			GameTimer:luaSetGameBegun(false)

			-- here we handle the --logic for if the game is set up to play multiple rounds
			-- we give the players X seconds between rounds and the center disc cylinder doesnt matter
			if (bMoreRoundsToPlay and not bGameWasRestarted and not bRunningGamemodeTests) then
				GameTimer:startCountdown(Gamemode:getNumberConfigVariable("TimeBetweenRounds") * 1000)
			else
				CenterDiscCylinder:showLua()
				CenterDiscCylinder:setTriggerCollision()
			end

			if primaryBall ~= nil then
				primaryBall:unFreeze(true)
			end
		end,
	},
	[State.GAME_BEGIN_ENTER] = {
		onEnter = function()
			primarySpawner:smoothResetBall()
			primaryBall:freeze(true)
			currentOvertimeCount = 0
			Gamemode:startGame()
			GameTimer:resetTimer()
			GameTimer:setRoundTime(Gamemode:getNumberConfigVariable("MatchLengthSeconds") * 1000)
			GameTimer:setClockBounce(true)
			bGameWasRestarted = false
			DriftballControlPanelBlueprint:setIsRestartable(true)
			--log("match starting")
		end,
		nextState = State.GAME_BEGIN,
		basicTimerLength = 3,
	},
	[State.GAME_BEGIN] = {
		onEnter = function()
			if primarySpawner == nil then
				------log("spawner is nil")
			else
				primarySpawner:addSmoothOffset(Vector.new(0, 0, -90))
			end
		end,
		nextState = State.GAME_START_COUNTDOWN,
		basicTimerLength = 1,
	},
    [State.GAME_START_COUNTDOWN] = {
        onEnter = function()
            GameScore:resetScore()

            GameTimer:StartTimerWithCountdown(15000)
        end,
    },
    [State.NEXT_POINT_COUNTDOWN] = {
        onEnter = function()
            GameTimer:startCountdown(15000)
        end,
    },
	[State.TEAM_SCORED_ENTER] = {
		onEnter = function()
            -- RedTimer:pauseTimer()
			-- GameTimer:pauseTimer()
            DisableGoalEffects()
			-- BasicTimer:start(1)
		end,
		nextState = State.TEAM_SCORED,
        basicTimerLength = 4
	},
	[State.TEAM_SCORED] = {
        onEnter = function()
            if primarySpawner == nil then
                ------log("spawner is nil")
            else
                primaryBall = primarySpawner:getSpawnedBall()
                if primaryBall == nil then
                    ------log("now ball is nil")
                end
            end
            if primaryBall ~= nil then
                primaryBall:unFreeze(true)
            end
		end,
		nextState = State.NEXT_POINT_COUNTDOWN,
        basicTimerLength = 1
	},
	[State.TIMER_ENDED] = {
		onEnter = function() end,
		nextState = State.GAME_EXIT,
		basicTimerLength = 3,
	},
    [State.OVERTIME_ENTER] = {
        onEnter = function()
			DisableGoalEffects()
        end,
		nextState = State.OVERTIME_COUNTDOWN,
        basicTimerLength = 4
	},
    [State.OVERTIME_COUNTDOWN] = {
        onEnter = function() 
            if primarySpawner == nil then
                ------log("spawner is nil")
            else
                primaryBall = primarySpawner:getSpawnedBall()
                if primaryBall == nil then
                    ------log("now ball is nil")
                end
            end
            if primaryBall ~= nil then
                primaryBall:unFreeze(true)
            end
            GameTimer:startCountdown(15000)
        end
	},
    [State.OVERTIME_RUNNING] = {
        onEnter = function()
			BlueTeamScoredCelebration:luaDisable()
			RedTeamScoredCelebration:luaDisable()
            EnableGoalEffects()
            RemoveDebuffs()
            if primarySpawner == nil then
                ------log("spawner is nil")
            else
                primaryBall = primarySpawner:getSpawnedBall()
                if primaryBall == nil then
                    ------log("now ball is nil")
                end
            end
            if primaryBall ~= nil then
                primaryBall:unFreeze(true)
            end
        end
	},

	[State.GAME_EXIT] = {
		onEnter = function()
			BlueTeamScoredCelebration:luaDisable()
			RedTeamScoredCelebration:luaDisable()
			if primarySpawner == nil then
                ------log("spawner is nil")
            else
                primaryBall = primarySpawner:getSpawnedBall()
                if primaryBall == nil then
                    ------log("now ball is nil")
                end
            end
            if primaryBall ~= nil then
                primaryBall:unFreeze(true)
            end
            isBallGrounded = false
			if (primarySpawner ~= nil) then
				primarySpawner:stopListeningForBallGrounded()
			end
			GameTimer:luaSetGameBegun(false)
			Gamemode:stopGame()

			DisableGoalEffects()
			RemoveDebuffs()
			--Invoke_ShowCenterDiscCylinder()
			GameTimer:pauseTimer()
			-- Invoke_OpenForceFieldA()
			if not bGameWasRestarted then
				HandleGameEndLogic()
			end

		end,
		nextState = State.BEGIN_PLAY,
        basicTimerLength = 2
	},  		
    [State.PRACTICE_START] = {
        onEnter = function()
            GameTimer:resetTimer()
           			
            GameScore:resetScore()
            
            GameTimer:setRoundTime(0)
            
			-- GameTimer:luaStartCountup()
        end,
        nextState = State.PRACTICE_RUNNING,
        basicTimerLength = 1
	},		
    [State.PRACTICE_ON_SCORED] = {
        onEnter = function()
        end,
		nextState = State.PRACTICE_FLING_BALL,
		basicTimerLength = 1,
	},
	[State.PRACTICE_FLING_BALL] = {
		onEnter = function() end,
		nextState = State.PRACTICE_RUNNING,
		basicTimerLength = 5,
	},
	[State.PRACTICE_RUNNING] = {
		onEnter = function() end,
	},
	[State.PRACTICE_ON_PASS_CAUGHT] = {
		onEnter = function() end,
	},
}

function SwitchState(state: number)
	BoostBuffToggleable:luaEnable()
	GameStateManager:updateGameState(state)
	currentState = state
	----log("Switching to state: " .. StateNames[state])
	stateMachineOnEnters[currentState].onEnter()
	local length = stateMachineOnEnters[currentState].basicTimerLength
	if length ~= nil then
		BasicTimer:start(length)
	end
end


function OnBallHitsMidfieldTrigger(disc: DiscEntity)
	if (disc ~= primarySpawner:getSpawnedBall()) then
		return
	end
	if currentState == State.NEXT_POINT_COUNTDOWN then
		GameTimer:skipCountdown()
	elseif (currentState == State.OVERTIME_COUNTDOWN) then
		GameTimer:skipCountdown()
    end
end

function HandleGameEndLogic(): ()

	if (bRunningGamemodeTests) then
		return
	end

	Gamemode:setQuestCompletedForAllPlayers("4FAB58404F3F5C8713FB11BB84EDE7C8")
	Gamemode:emitStatEventForAllPlayers("Sub_TKB_TackleballRoundPlayed", 1)

	if (team0points > team1points) then
		team0RoundsWon += 1
		----log("team0 won a round, bringing their rounds won to " .. team0RoundsWon)
		Gamemode:setQuestCompletedForAllPlayersOnTeam(0, "39B70EAD4AC71ED7704F18B434A6D401")
		Gamemode:emitStatEventForAllPlayersOnTeam(0, "Sub_TKB_TackleballRoundWon", 1)
	else
		team1RoundsWon += 1
		----log("team1 won a round, bringing their rounds won to " .. team1RoundsWon)
		Gamemode:setQuestCompletedForAllPlayersOnTeam(1, "39B70EAD4AC71ED7704F18B434A6D401")
		Gamemode:emitStatEventForAllPlayersOnTeam(1, "Sub_TKB_TackleballRoundWon", 1)
	end



	local RoundsPerMatch = Gamemode:getNumberConfigVariable("RoundsPerMatch")
	local TeamWhoLostMatch = -1
	if (Gamemode:getBoolConfigVariable("bUseBestOf")) then
		if (team0RoundsWon > (RoundsPerMatch/2)) then
			----log("team0 won the best of " .. RoundsPerMatch)
			TeamWhoLostMatch = 1
			team0RoundsWon = 0
			team1RoundsWon = 0
		elseif (team1RoundsWon > (RoundsPerMatch/2)) then
			----log("team1 won the best of " .. RoundsPerMatch)
			TeamWhoLostMatch = 0
			team0RoundsWon = 0
			team1RoundsWon = 0
		elseif (team0RoundsWon + team1RoundsWon == RoundsPerMatch) then
			----log("teams are tied at " .. team0RoundsWon)
			TeamWhoLostMatch = -1
			team0RoundsWon = 0
			team1RoundsWon = 0
		end
	else
		if (team0RoundsWon + team1RoundsWon == RoundsPerMatch) then
			if (team0RoundsWon > team1RoundsWon) then
				----log("team0 won the 'allroundsplayed' with " .. team0RoundsWon)
				TeamWhoLostMatch = 1
				team0RoundsWon = 0
				team1RoundsWon = 0
			elseif (team1RoundsWon > team0RoundsWon) then
				----log("team1 won the 'allroundsplayed' with " .. team1RoundsWon)
				TeamWhoLostMatch = 0
				team0RoundsWon = 0
				team1RoundsWon = 0
			else
				----log("teams are tied at " .. team0RoundsWon)
				TeamWhoLostMatch = -1
				team0RoundsWon = 0
				team1RoundsWon = 0
			end
		elseif (RoundsPerMatch == 0) then
			----log("error - rounds per match are 0")
			team0RoundsWon = 0
			team1RoundsWon = 0
		end
	end
	if (TeamWhoLostMatch ~= -1) then
		bMoreRoundsToPlay = false
		-- handle quests and stats when completing a match
		Gamemode:setQuestCompletedForAllPlayers("B4275C4649DD15C2D9A2F78E58021640")
		Gamemode:emitStatEventForAllPlayers("Sub_TKB_TackleballGamePlayed", 1)
		--

		local bKickLosingTeam = Gamemode:getBoolConfigVariable("bKickLosingTeam")

		if (TeamWhoLostMatch == 1) then
			Gamemode:setQuestCompletedForAllPlayersOnTeam(0, "75A4CCC7451F4A0ECD75F99431924107")
			Gamemode:emitStatEventForAllPlayersOnTeam(0, "Sub_TKB_TackleballGameWon", 1)
			if bKickLosingTeam then
				AwayTeamKickOutVolume:luaEnable()
			end
		elseif (TeamWhoLostMatch == 0) then
			Gamemode:setQuestCompletedForAllPlayersOnTeam(1, "75A4CCC7451F4A0ECD75F99431924107")
			Gamemode:emitStatEventForAllPlayersOnTeam(1, "Sub_TKB_TackleballGameWon", 1)
			if bKickLosingTeam then
				HomeTeamKickOutVolume:luaEnable()
			end
		end
		if (not bKickLosingTeam and Gamemode:getBoolConfigVariable("bShuffleTeamsAfterMatch")) then
			Gamemode:shuffleTeams()
		end
	else
		bMoreRoundsToPlay = true
	end

	Gamemode:setTeamRoundsWon(0, team0RoundsWon)
	Gamemode:setTeamRoundsWon(1, team1RoundsWon)

end


function Debug_MoveBall() end

function BallNowGrounded()
	isBallGrounded = true
	if waitForBallGrounded then
		waitForBallGrounded = false
		if (primarySpawner ~= nil) then
			primarySpawner:stopListeningForBallGrounded()
		end
		OnTimerFinished()
	else
		----log("ball was sending grounded events, but we weren't waiting for the ball to be grounded in luau, so we manually set listen to false")
		----log("how did we get here?")
		if (primarySpawner ~= nil) then
			primarySpawner:stopListeningForBallGrounded()
		end
	end
end



function BasicTimerFinished()
	if primarySpawner == nil then
		------log("spawner is nil")
	else
		primaryBall = primarySpawner:getSpawnedBall()
		if primaryBall == nil then
			------log("now ball is nil")
		end
	end
	if hasShotDiscWaitingForCountdown then
        hasShotDiscWaitingForCountdown = false;
	end
	------log("Basic timer finished at: " .. StateNames[currentState])
	if stateMachineOnEnters[currentState].nextState then
		SwitchState(stateMachineOnEnters[currentState].nextState)
	end
end



function OnTimerCountdownFinished(): ()
	if primarySpawner == nil then
		------log("spawner is nil")
	else
		primaryBall = primarySpawner:getSpawnedBall()
		if primaryBall == nil then
			------log("now ball is nil")
		end
	end
	if currentState == State.GAME_START_COUNTDOWN then
        primaryBall:unFreeze(true);
		------log("TimerCountdownFinished and we are in Game_Start_Countdown")
		local randomX = math.random(0, 2 * math.pi)
		------log("local randomX = math.random(0, 2*math.pi)")
		local randomY = math.random(0, 2 * math.pi)
		------log("local randomY = math.random(0, 2*math.pi)")
		local b = math.tan(math.rad(15))
		------log("local b = math.tan(math.rad(15))")
		local randomVector = Vector.new(math.cos(randomX), math.sin(randomY), 0)
		------log("local randomVector = Vec3(math.cos(randomX), math.sin(randomY),0)")
		local z = Vector.new(0, 0, 1)
		------log("local z = Vec3(0,0,1)")
		randomVector = randomVector * b
		------log("randomVector = randomVector * b")
		randomVector = randomVector + z
		------log("randomVector = randomVector + z")
		--randomVector:normalize(1)
		------log("--randomVector:normalize(1)")
		local randomVelocity = randomVector * 3500
		------log("local randomVelocity = randomVector * 3500")
		if primaryBall ~= nil then
			primaryBall:setVelocity(randomVelocity)
		end
		hasShotDiscWaitingForCountdown = true
		------log("hasShotDiscWaitingForCountdown = true")

		CenterDiscCylinder:hideLua()
		CenterDiscCylinder:disableCollisionLua()

		------log("CenterDiscCylinder:hideLua()")
		BasicTimer:start(1)
		------log("Invoke_StartBasicTimer(1)")
	end
	if countdownFinishedDirections[currentState] then
		SwitchState(countdownFinishedDirections[currentState])
	end
end


-- ====== Input events ======
function OnTeamScored(goalInfo: GoalInfo): ()
	
    if primarySpawner == nil then
        ------log("spawner is nil")
    else
        primaryBall = primarySpawner:getSpawnedBall()
        if primaryBall == nil then
            ------log("now ball is nil")
        end
    end

    -- keep track of points in luau so we don't have to request them from netvars every time
    local teamNum = goalInfo.scoringTeam
    if (teamNum == 0) then
        team0points += goalInfo.goalPoints
		Gamemode:setTeamScore(0, team0points)
		RedTeamScoredCelebration:luaEnable()
    elseif (teamNum == 1) then
        team1points += goalInfo.goalPoints
		Gamemode:setTeamScore(1, team1points)
		BlueTeamScoredCelebration:luaEnable()
    end

	-- handle statistics and quests for shooting
	if (goalInfo.playerScoredID ~= -1 and goalInfo.playerScoredTeam == goalInfo.scoringTeam) then
		local Pawn = VRPawn.getPlayerByID(goalInfo.playerScoredID)
		if (Pawn ~= nil) then
			Pawn:client_EmitStatEvent("Sub_TKB_GoalScored", 1)
		end
	end
	
	-- only let the clock "bounce" if the scores are equal
	-- the default is that the clock will bounce (scores of 0,0)
	if (team0points == team1points) then
		GameTimer:setClockBounce(true)
	else
		GameTimer:setClockBounce(false)
	end


    if currentState == State.PRACTICE_RUNNING then

		-- this actually spawns the boost buff which
		-- just does an IsOverlapping check on all
		-- actors one time
		BoostBuffToggleable:luaEnable()

        GameScore:incrementScore(goalInfo)
        --SwitchState(State.PRACTICE_ON_SCORED)
    elseif currentState == State.RUNNING then

		-- this actually spawns the boost buff which
		-- just does an IsOverlapping check on all
		-- actors one time
		BoostBuffToggleable:luaEnable()

        -- apply debuffs and buffs based on who scored
        if teamNum == 1 then 
            ApplyTeam1Debuff()
            ShowDebuffVolume2()
        end
        if teamNum == 0 then 
            ApplyTeam0Debuff()
            ShowDebuffVolume()
        end
        -- increment the score on the cpp side
        GameScore:incrementScore(goalInfo)

        -- freeze ball for a very short time
        if primaryBall ~= nil then
            primaryBall:freeze(true)
            primaryBall:setVelocity(Vector.new(0,0,0))
        end

		local MercyScoreDifference = Gamemode:getNumberConfigVariable("MercyScoreDifference")
        -- check for mercy rule to instantly end game
        if ((team0points >= team1points + MercyScoreDifference) or (team1points >= team0points + MercyScoreDifference)) then
            SwitchState(State.GAME_EXIT)
            return
        end


        if (waitForBallGrounded) then 
			if (primarySpawner ~= nil) then
            	primarySpawner:stopListeningForBallGrounded()
			end
            waitForBallGrounded = false
            isBallGrounded = false
			-- if we were waiting for the ball to be grounded but then we scored
			-- check if we should go into overtime or just end the game
            if (team0points == team1points) then
                SwitchState(State.OVERTIME_ENTER)
                return
			else
				SwitchState(State.GAME_EXIT)
				return
			end
        end

		SwitchState(State.TEAM_SCORED_ENTER)
	elseif currentState == State.OVERTIME_RUNNING then

		-- this actually spawns the boost buff which
		-- just does an IsOverlapping check on all
		-- actors one time
		BoostBuffToggleable:luaEnable()

		if primaryBall ~= nil then
			primaryBall:freeze(true)
		end

		GameTimer:pauseTimer()

		GameScore:incrementScore(goalInfo)

		SwitchState(State.GAME_EXIT)
	else
		------log("Scored a goal when there is no match running")
	end
end


function Delegate_TogglePracticeMode(): ()
	if
		currentState == State.PRACTICE_RUNNING
		or currentState == State.PRACTICE_START
		or currentState == State.PRACTICE_ON_SCORED
		or currentState == State.PRACTICE_FLING_BALL
	then
		GameTimer:pauseTimer()
		--Invoke_OpenForceFieldA()
		SwitchState(State.GAME_EXIT)
		------log("EXITED PRACTICE MODE")
	else
		SwitchState(State.PRACTICE_START)
		------log("BEGIN PRACTICE MODE")
	end
end

function Delegate_OnEndGameButtonPress(): ()
	SwitchState(State.GAME_EXIT)
end

function OnBallGotHit(disc: DiscEntity, data: LuauPhysicsObjectData): ()

	----log("ballgothit")

	local Pawn = VRPawn.getPlayerByID(disc.rollbackRecentPlayerHit)

	if (processingRecentHit or (currentState ~= State.RUNNING and currentState ~= State.OVERTIME_RUNNING)) then
		----log("processingRecentHit or (currentState ~= State.RUNNING and currentState ~= State.OVERTIME_RUNNING)")
		return
	end

	local WasSaved = CheckIfHitIsSave(disc, data)

	if (WasSaved) then
		----log("WHAT A SAVE!!!!")
		if (not processingRecentHit) then
			GameStateManagerBP:PlaySaveEvent()
		end
		HandleBallHitEventCooldown()

		if (Pawn ~= nil) then
			Pawn:client_EmitStatEvent("Sub_TKB_TackleballBallSaved", 1)
			Pawn:client_SetQuestCompleted("FA309E80402D8114A8F009B28AFA191E")
		end

		return
	end
	-- things like shots and clears require us to wait a lil bit first
	Promise.new(function () 
		LuauClock.timeout(0.75):await()
		local WasShot = CheckIfHitIsShot(disc, data)
		if (WasShot) then
			----log("WHAT A SHOT!!!!")
			HandleBallHitEventCooldown()

			if (Pawn ~= nil) then
				Pawn:client_EmitStatEvent("Sub_TKB_TackleballBallShot", 1)
				Pawn:client_SetQuestCompleted("64707D4748449147034DC4AF2C0559B8")
			end

			return
		end

		local WasClear = CheckIfHitIsClear(disc, data)
		if (WasClear) then
			----log("WHAT A CLEAR!!!!")
			if (not processingRecentHit) then
				GameStateManagerBP:PlayClearEvent()
			end
			HandleBallHitEventCooldown()

			if (Pawn ~= nil) then
				Pawn:client_EmitStatEvent("Sub_TKB_TackleballBallClear", 1)
				Pawn:client_SetQuestCompleted("F928AEDA44533FD15C0C06AA9D596F1D")
			end

			return
		end
		LuauClock.timeout(2):await()
		local WasTurnover = CheckIfHitIsTurnover(disc, data)
		if (WasTurnover) then
			if (not processingRecentHit) then
				GameStateManagerBP:PlayTurnoverEvent()
			end
			HandleBallHitEventCooldown()

			if (Pawn ~= nil) then
				Pawn:client_EmitStatEvent("Sub_TKB_TackleballBallSteal", 1)
				Pawn:client_SetQuestCompleted("508A1D5F4F3C6EEF35F568B8B1BF940F")
			end

			return
		end
	end )
end

function CheckIfHitIsTurnover(disc: DiscEntity, data: LuauPhysicsObjectData): (boolean)
	local TeamWhoHitPreCheck = data.teamRecentHit
	local TeamWhoHitEvenBefore = data.teamLastHit
	if (TeamWhoHitEvenBefore == TeamWhoHitPreCheck) then
		return false
	end
	if (VRPawn.getTeamIndexByID(disc.rollbackRecentPlayerHit) == TeamWhoHitPreCheck) then
		----log("teams were not equal")
		return false
	end
	local DirectionFromPlayerToBall = disc:getDiscPosition() - VRPawn.getPositionByID(disc.rollbackRecentPlayerHit)
	local DistanceBetweenPlayerAndBall = math.sqrt((DirectionFromPlayerToBall.x*DirectionFromPlayerToBall.x)+(DirectionFromPlayerToBall.y*DirectionFromPlayerToBall.y)+(DirectionFromPlayerToBall.z*DirectionFromPlayerToBall.z))
	if (DistanceBetweenPlayerAndBall > 200) then
		return false
	end
	return true
end

function CheckIfHitIsClear(disc: DiscEntity, data: LuauPhysicsObjectData): (boolean)
	local TeamWhoHitPreCheck = data.teamRecentHit
	local GoalPos = Vec3.new(0,0,0)
	local GoalForwardVector = Vec3.new(0,0,0)
	if (VRPawn.getTeamIndexByID(disc.rollbackRecentPlayerHit) ~= TeamWhoHitPreCheck) then
		----log("teams were not equal")
		return false
	elseif (TeamWhoHitPreCheck == 1) then
		GoalPos = RedGoal:getPosition()
		GoalForwardVector = RedGoal:getForwardVectorLuau()
	else
		GoalPos = BlueGoal:getPosition()
		GoalForwardVector = BlueGoal:getForwardVectorLuau()
	end

	local DirectionFromPlayerToBall = disc:getDiscPosition() - VRPawn.getPositionByID(disc.rollbackRecentPlayerHit)
	local DistanceBetweenPlayerAndBall = math.sqrt((DirectionFromPlayerToBall.x*DirectionFromPlayerToBall.x)+(DirectionFromPlayerToBall.y*DirectionFromPlayerToBall.y)+(DirectionFromPlayerToBall.z*DirectionFromPlayerToBall.z))
	if (DistanceBetweenPlayerAndBall < 500) then
		return false
	end
	local DirectionFromBallToEnemyGoal = (GoalPos - disc:getDiscPosition())
	local DistanceBetweenEnemyGoalAndBall = math.sqrt((DirectionFromBallToEnemyGoal.x*DirectionFromBallToEnemyGoal.x)+(DirectionFromBallToEnemyGoal.y*DirectionFromBallToEnemyGoal.y)+(DirectionFromBallToEnemyGoal.z*DirectionFromBallToEnemyGoal.z))
	if (DistanceBetweenEnemyGoalAndBall < 3000) then
		----log("distance less than 4k")
		return false
	end
	local CurrentBallVelocity = disc:getDiscVelocity()

	local BallVelocityMagnitude = math.sqrt((CurrentBallVelocity.x*CurrentBallVelocity.x)+(CurrentBallVelocity.y*CurrentBallVelocity.y)+(CurrentBallVelocity.z*CurrentBallVelocity.z))

	if (BallVelocityMagnitude <= 450) then
		----log(BallVelocityMagnitude)
		----log("ball velocity less than 700")
		return false
	end

	local NormalizedBallVelocity = CurrentBallVelocity / BallVelocityMagnitude

	local DirectionAndBallVelocityDotProduct = ((NormalizedBallVelocity.x * GoalForwardVector.x) + (NormalizedBallVelocity.y * GoalForwardVector.y) + (NormalizedBallVelocity.z * GoalForwardVector.z))

	if (DirectionAndBallVelocityDotProduct < 0) then
		----log(DirectionAndBallVelocityDotProduct)
		----log("dot product wrong")
		return false
	end
	return true
end

function CheckIfHitIsShot(disc: DiscEntity, data: LuauPhysicsObjectData): (boolean)
	local TeamWhoHitPreCheck = data.teamRecentHit
	local GoalPos = Vec3.new(0,0,0)
	local GoalForwardVector = Vec3.new(0,0,0)
	if (VRPawn.getTeamIndexByID(disc.rollbackRecentPlayerHit) ~= TeamWhoHitPreCheck) then
		--log("teams were not equal")
		return false
	elseif (TeamWhoHitPreCheck == 1) then
		GoalPos = RedGoal:getPosition()
		GoalForwardVector = RedGoal:getForwardVectorLuau()
	else
		GoalPos = BlueGoal:getPosition()
		GoalForwardVector = BlueGoal:getForwardVectorLuau()
	end

	local DirectionFromPlayerToBall = disc:getDiscPosition() - VRPawn.getPositionByID(disc.rollbackRecentPlayerHit)
	local DistanceBetweenPlayerAndBall = math.sqrt((DirectionFromPlayerToBall.x*DirectionFromPlayerToBall.x)+(DirectionFromPlayerToBall.y*DirectionFromPlayerToBall.y)+(DirectionFromPlayerToBall.z*DirectionFromPlayerToBall.z))
	if (DistanceBetweenPlayerAndBall < 300) then
		return false
	end

	local DirectionFromBallToEnemyGoal = (GoalPos - disc:getDiscPosition())
	local DistanceBetweenEnemyGoalAndBall = math.sqrt((DirectionFromBallToEnemyGoal.x*DirectionFromBallToEnemyGoal.x)+(DirectionFromBallToEnemyGoal.y*DirectionFromBallToEnemyGoal.y)+(DirectionFromBallToEnemyGoal.z*DirectionFromBallToEnemyGoal.z))
	if (DistanceBetweenEnemyGoalAndBall > 3000) then
		--log("distance greater tahn 5k")
		return false
	end
	local NormalizedDirection = DirectionFromBallToEnemyGoal / DistanceBetweenEnemyGoalAndBall

	local CurrentBallVelocity = disc:getDiscVelocity()

	local BallVelocityMagnitude = math.sqrt((CurrentBallVelocity.x*CurrentBallVelocity.x)+(CurrentBallVelocity.y*CurrentBallVelocity.y)+(CurrentBallVelocity.z*CurrentBallVelocity.z))

	if (BallVelocityMagnitude <= 450) then
		--log(BallVelocityMagnitude)
		--log("ball velocity less than 700")
		return false
	end

	local NormalizedBallVelocity = CurrentBallVelocity / BallVelocityMagnitude

	local DirectionAndBallVelocityDotProduct = ((NormalizedBallVelocity.x * NormalizedDirection.x) + (NormalizedBallVelocity.y * NormalizedDirection.y) + (NormalizedBallVelocity.z * NormalizedDirection.z))
	local ForwardVectorAndBallVelocityDotProduct = ((NormalizedBallVelocity.x * GoalForwardVector.x) + (NormalizedBallVelocity.y * GoalForwardVector.y) + (NormalizedBallVelocity.z * GoalForwardVector.z))

	if (DirectionAndBallVelocityDotProduct < 0.825 or ForwardVectorAndBallVelocityDotProduct < 0) then
		--log(DirectionAndBallVelocityDotProduct)
		--log("dot product wrong")
		return false
	end

	if (not processingRecentHit) then
		if (BallVelocityMagnitude > 1400) then
			log(BallVelocityMagnitude)
			GameStateManagerBP:PlayEspeciallyGoodShotEvent()
		else
			GameStateManagerBP:PlayShotEvent()
		end
	end

	return true
end

function CheckIfHitIsSave(disc: DiscEntity, data: LuauPhysicsObjectData): (boolean)

	if (data.teamRecentHit == data.teamLastHit) then
		return false
	end

	local GoalPos = Vec3.new(0,0,0)

	if (data.teamRecentHit == 1) then
		GoalPos = BlueGoal:getPosition()
	else
		GoalPos = RedGoal:getPosition()
	end


	local DirectionFromBallToFriendlyGoal = (GoalPos - data.position)

	local DistanceBetweenFriendlyGoalAndBall = math.sqrt((DirectionFromBallToFriendlyGoal.x*DirectionFromBallToFriendlyGoal.x)+(DirectionFromBallToFriendlyGoal.y*DirectionFromBallToFriendlyGoal.y)+(DirectionFromBallToFriendlyGoal.z*DirectionFromBallToFriendlyGoal.z))

	if (DistanceBetweenFriendlyGoalAndBall > 1500) then
		return false
	end

	local PlayerHitPosition = VRPawn.getPositionByID(disc.rollbackRecentPlayerHit)
	local DirectionFromPlayerToFriendlyGoal = (GoalPos - PlayerHitPosition)
	local DistanceBetweenFriendlyGoalAndPlayerWhoHit = math.sqrt((DirectionFromPlayerToFriendlyGoal.x*DirectionFromPlayerToFriendlyGoal.x)+(DirectionFromPlayerToFriendlyGoal.y*DirectionFromPlayerToFriendlyGoal.y)+(DirectionFromPlayerToFriendlyGoal.z*DirectionFromPlayerToFriendlyGoal.z))

	if (DistanceBetweenFriendlyGoalAndPlayerWhoHit > 1500) then
		return false
	end

	local NormalizedDirection = DirectionFromBallToFriendlyGoal / DistanceBetweenFriendlyGoalAndBall

	local BallVelocityMagnitude = math.sqrt((data.velocity.x*data.velocity.x)+(data.velocity.y*data.velocity.y)+(data.velocity.z*data.velocity.z))

	if (BallVelocityMagnitude <= 100) then
		return false
	end

	local NormalizedBallVelocity = data.velocity / BallVelocityMagnitude

	local DirectionAndBallVelocityDotProduct = ((NormalizedBallVelocity.x * NormalizedDirection.x) + (NormalizedBallVelocity.y * NormalizedDirection.y) + (NormalizedBallVelocity.z * NormalizedDirection.z))

	if (DirectionAndBallVelocityDotProduct < 0.835) then
		return false
	end

	return true
end

function HandleBallHitEventCooldown(): () 
	Promise.new(function () 
		processingRecentHit = true
		LuauClock.timeout(5):await()
		processingRecentHit = false
	end )
end


function SkipGameStateBasicTimer()
	BasicTimer:stopAndResetTimer()
	BasicTimerFinished()
end

function RestartButtonHitEvent()
	if(currentState~=State.GAME_EXIT and (currentState~=State.WAITING_TO_START or (currentState==State.WAITING_TO_START and bMoreRoundsToPlay)) and currentState~=State.NOT_RUNNING and bGameWasRestarted == false) then
		bGameWasRestarted = true;
		SwitchState(State.GAME_EXIT)
		--log("game restarted")
		DriftballControlPanelBlueprint:setIsRestartable(false)
	end
end


function PracticeModeButtonHitEvent()
	if(currentState==State.WAITING_TO_START and not bPracticeModeEnabled and not bMoreRoundsToPlay) then
		--log("practice mode on")
		DriftballControlPanelToggle:luaEnable()
		CenterDiscCylinder:hideLua()
		CenterDiscCylinder:disableCollisionLua()
		bPracticeModeEnabled = true
	elseif(currentState==State.WAITING_TO_START and bPracticeModeEnabled) then
		--log("practice mode off")
		DriftballControlPanelToggle:luaDisable()
		CenterDiscCylinder:showLua()
		CenterDiscCylinder:setTriggerCollision()
		bPracticeModeEnabled = false
	end
end

-- Called when this script starts up
function BeginPlay()
	
	DisableGoalEffects()

	Gamemode:setTeamScore(0, 0)
	Gamemode:setTeamScore(1, 0)

	Gamemode:setTeamRoundsWon(0, 0)
	Gamemode:setTeamRoundsWon(1, 0)

	RoundsToPlay = Gamemode:getNumberConfigVariable("RoundsPerMatch")
	bUseBestOf = Gamemode:getBoolConfigVariable("bUseBestOf")

	BasicTimer.finished.Listen(BasicTimerFinished)
	primarySpawner.onBallGrounded.Listen(BallNowGrounded)
	primarySpawner.playerHitLuau.Listen(OnBallGotHit)
	GameTimer.onCountdownEnd.Listen(OnTimerCountdownFinished)
	GameTimer.onTimerStopped.Listen(OnTimerFinished)
	GameTimer.enterGameState.Listen(SwitchState)
	RedGoal.onGoalScored.Listen(OnTeamScored)
	BlueGoal.onGoalScored.Listen(OnTeamScored)
	CenterDiscCylinder.onOverlapByDisc.Listen(OnBallHitsStartTrigger)
	MidfieldDiscPhysical.onTwoWayOverlapByDisc.Listen(OnBallHitsMidfieldTrigger)
	MidfieldDiscPhysical2.onTwoWayOverlapByDisc.Listen(OnBallHitsMidfieldTrigger)
	GameStateManager.skipGameState.Listen(SkipGameStateBasicTimer)
	GameStateManager.forceBallHitStartTrigger.Listen(OnForceBallHitsStartTrigger)
	DriftballControlPanelComp.practiceModeButtonHit.Listen(PracticeModeButtonHitEvent)
	DriftballControlPanelComp.restartButtonHit.Listen(RestartButtonHitEvent)

	SetupForPlay()
	SwitchState(State.WAITING_TO_START)
end