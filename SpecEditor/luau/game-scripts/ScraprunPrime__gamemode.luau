-- External Definitions, Please do not touch
-- =========================================================================
local RunnerTeamChanger: TeamChangeComponent = nil
local CheckpointBlocker: PhysicalComponent = nil
local SpectatorSpleefCheckpointBlocker: PhysicalComponent = nil
local SpleefLockInBlocker: PhysicalComponent = nil
local KillerTeamChanger: TeamChangeComponent = nil
local GameScore: ScoreComponent = nil
local GameTimer: GameTimeComponent = nil
local GameStateManager: GameStateManagerComponent = nil
local BasicTimer: TimerComponent = nil
local ResetTrigger: ToggleableComponent = nil
local ResetTriggerTeleporter: DeathrunResetTeleporterComponent = nil
local Forcefield1: PhysicalComponent = nil
local Forcefield2: PhysicalComponent = nil
local TeamSwitcherBlocker: PhysicalComponent = nil
local KillerForcefield: PhysicalComponent = nil
local TrapTrapDoor: ToggleableComponent = nil
local TrapTrapDoor2: ToggleableComponent = nil
local TrapTrapDoor3: ToggleableComponent = nil
local TrapSpinner: TrapSpinnerComponent = nil
local TrapSpinner2: TrapSpinnerComponent = nil
local TrapSpinner3: TrapSpinnerComponent = nil
local TrapSpinner4: TrapSpinnerComponent = nil
local TrapGrabHandles: ToggleableComponent = nil
local TrapFallingBlock: ToggleableComponent = nil
local TrapFallingBlock2: ToggleableComponent = nil
local TrapFallingBlock3: ToggleableComponent = nil
local TrapFallingBlock4: ToggleableComponent = nil
local TrapFallingBlock5: ToggleableComponent = nil
local TrapFallingBlock6: ToggleableComponent = nil
local TrapFan: ToggleableComponent = nil
local TrapStickySlime: ToggleableComponent = nil
local TrapStickySlime2: ToggleableComponent = nil
local TrapBunnyHop1: ToggleableComponent = nil
local TrapBunnyHop2: ToggleableComponent = nil
local TrapBunnyHop3: ToggleableComponent = nil
local TrapBunnyHop4: ToggleableComponent = nil
local TrapBunnyHop5: ToggleableComponent = nil
local TrapBunnyHop6: ToggleableComponent = nil
local TrapBunnyHop7: ToggleableComponent = nil
local TrapBunnyHop8: ToggleableComponent = nil
local TrapBunnyHop9: ToggleableComponent = nil
local TrapBunnyHop10: ToggleableComponent = nil
local TrapBunnyHop11: ToggleableComponent = nil
local TrapBunnyHop12: ToggleableComponent = nil
local SpleefBlocker: PhysicalComponent = nil
local SpleefBlocker2: PhysicalComponent = nil
local StartButton: BasicButtonComponent = nil
local Trap1Button: BasicButtonComponent = nil
local Trap2Button: BasicButtonComponent = nil
local Trap2Button2: BasicButtonComponent = nil
local Trap3Button: BasicButtonComponent = nil
local Trap4Button: BasicButtonComponent = nil
local Trap5Button: BasicButtonComponent = nil
local Trap6Button: BasicButtonComponent = nil
local Trap9Button: BasicButtonComponent = nil
local Trap9Button2: BasicButtonComponent = nil
local Trap10Button: BasicButtonComponent = nil
local Trap11Button: BasicButtonComponent = nil
local EndTrigger: PhysicalComponent = nil
local TrapRotatingPlatform1: TrapRotatingPlatformComponent = nil
local TrapRotatingPlatform2: TrapRotatingPlatformComponent = nil
local TrapRotatingPlatform3: TrapRotatingPlatformComponent = nil
local TrapSlidingPlatform1: SlidingPlatformComponent = nil
local TrapSlidingPlatform2: SlidingPlatformComponent = nil
local TrapSlidingPlatform3: SlidingPlatformComponent = nil
local TrapSlidingPlatform4: SlidingPlatformComponent = nil
local TrapSlidingPlatform5: SlidingPlatformComponent = nil
local TrapSlidingPlatform6: SlidingPlatformComponent = nil
local TrapSlidingPlatform7: SlidingPlatformComponent = nil
local TrapSlidingPlatform8: SlidingPlatformComponent = nil
local TrapDeathBall: BallSpawnerComponent = nil
local PlinkoBall1: BallSpawnerComponent = nil
local PlinkoBall2: BallSpawnerComponent = nil
local PlinkoBall3: BallSpawnerComponent = nil
local PlinkoBall4: BallSpawnerComponent = nil
local PlinkoBall5: BallSpawnerComponent = nil
local PlinkoBall6: BallSpawnerComponent = nil
local PlinkoStartButton: BasicButtonComponent = nil
local CheckpointLightSwitch1: ToggleableComponent = nil
local CheckpointLightSwitch2: ToggleableComponent = nil
local CheckpointLightSwitch3: ToggleableComponent = nil
local CheckpointBlocker2: PhysicalComponent = nil
local KillerlessRunLightSwitch: ToggleableComponent = nil
local SecretLightSwitch1: ToggleableComponent = nil
local SecretLightSwitch2: ToggleableComponent = nil
local SecretLightSwitch3: ToggleableComponent = nil
local SecretDoor: PhysicalComponent = nil
local ScraprunBlocker: PhysicalComponent = nil
local KickEveryoneOutTeleporter: aa_se_BP_TeleporterComponent = nil

killerTeamSizeOnGameStart = 0
runnerTeamSizeOnGameStart = 0
bWasGameRunning = false
Trap1Array = {}
Trap3FallingBlockArray = {}
Trap7BunnyhopArray = {}
Trap10SlidingPlatformArray = {}
PlinkoBallsArray = {}

bCanGameEndFromTimer = false;

Checkpoint1Active = false
Checkpoint2Active = false
Checkpoint3Active = false

props = {
	matchLengthSeconds = 0,
	mercyWinDifference = 12,
	overtimeLengthSeconds = 0,
    __call = function()
        return 15
    end
}

-- =========================================================================

-- currentState is the currentState of the gamemode
State = {
	NOT_RUNNING = 0, -- Game is not running

	WAITING_TO_START = 1, -- Server just started up, Wait 20 seconds before starting the game

	GAME_BEGIN_ENTER = 2, -- Game is waiting to start

	GAME_BEGIN = 3,

	RUNNING = 5, -- Running

	TEAM_SCORED_ENTER = 6, -- Team has scored, Wait

	TEAM_SCORED = 7, -- Team has scored, Wait

    GAME_EXIT = 10,
    OVERTIME_COUNTDOWN = 12,
	OVERTIME_RUNNING = 13,
	BEGIN_PLAY = 14, -- Server just started up, Wait 20 seconds before starting the game

	PRACTICE_START = 15,
	PRACTICE_ON_SCORED = 16,
	PRACTICE_FLING_BALL = 17,
    PRACTICE_RUNNING = 18,
    PRACTICE_ON_PASS_CAUGHT = 19,
}

StateNames = {
	[State.NOT_RUNNING] = "NOT_RUNNING",
	[State.WAITING_TO_START] = "WAITING_TO_START",
	[State.GAME_BEGIN_ENTER] = "GAME_BEGIN_ENTER",
	[State.GAME_BEGIN] = "GAME_BEGIN",
	[State.RUNNING] = "RUNNING",
	[State.TEAM_SCORED_ENTER] = "TEAM_SCORED_ENTER",
	[State.TEAM_SCORED] = "TEAM_SCORED",
	[State.BEGIN_PLAY] = "BEGIN_PLAY",
	[State.PRACTICE_START] = "PRACTICE_START",
	[State.PRACTICE_ON_SCORED] = "PRACTICE_ON_SCORED",
	[State.PRACTICE_FLING_BALL] = "PRACTICE_FLING_BALL",
    [State.PRACTICE_RUNNING] = "PRACTICE_RUNNING",
    [State.PRACTICE_ON_PASS_CAUGHT] = "PRACTICE_ON_PASS_CAUGHT",
}

countdownFinishedDirections = {
    [State.GAME_BEGIN] = State.RUNNING,
    [State.TEAM_SCORED_ENTER] = State.GAME_EXIT,
    [State.OVERTIME_COUNTDOWN] = State.OVERTIME_RUNNING
}

currentState = State.NOT_RUNNING
MAX_OVERTIME_COUNT = 2
currentOvertimeCount = 0
usingForcefield = "F"
trapDoorsActive = false
plinkoActive = false

----

function OnStartGameButtonPress(): ()
    if currentState == 1 and ScraprunOpen then
        KillerForcefield:showLua()
        KillerForcefield:setDefaultCollision()
        Forcefield2:hideLua()
        Forcefield2:disableCollisionLua()
        SwitchState(State.GAME_BEGIN)
    end
end

function Debug_OnGameStart(): ()
	OnGameStart()
end
function OnGameStart()
    SwitchState(State.GAME_START_COUNTDOWN)
end

stateMachineOnEnters = {
	[State.NOT_RUNNING] = nil,
    [State.BEGIN_PLAY] = {
        onEnter = function() 
            GameTimer:pauseTimer()
	        GameTimer:setRoundTime(Gamemode:getNumberConfigVariable("MatchLengthSeconds") * 1000)
            GameTimer:resetTimer()
            EnableForcefields()
            -- this teleporter kicks everyone out who is part of the gamemode still when the game is ready to start again
            KickEveryoneOutTeleporter:setTeleportLocationRelativeToModuleSlot(Vector.new(-4350,-3750,1175))
            Promise.new(function () 
                KickEveryoneOutTeleporter:activateTeleporter()
                LuauClock.timeout(0.25):await()
                KickEveryoneOutTeleporter:deActivateTeleporter()
            end ) 
            SpleefBlocker:showLua()
            SpleefBlocker:setDefaultCollision()
            SpleefBlocker2:showLua()
            SpleefBlocker2:setDefaultCollision()
        end,
        nextState = State.WAITING_TO_START,
        basicTimerLength = 10
    },
	[State.WAITING_TO_START] = {
        onEnter = function()
            TeamSwitcherBlocker:hideLua()
            TeamSwitcherBlocker:disableCollisionLua()
            GameTimer:luaSetGameBegun(false)
            TrapSpinner:setCanMove(false)
            TrapSpinner2:setCanMove(false)
            TrapSpinner3:setCanMove(false)
            TrapSpinner4:setCanMove(false)
            TrapRotatingPlatform1:setCanMove(false)
            TrapRotatingPlatform2:setCanMove(false)
            TrapRotatingPlatform3:setCanMove(false)
            for i in Trap10SlidingPlatformArray do
                Trap10SlidingPlatformArray[i]:setCanMove(false)
            end
            KickEveryoneOutTeleporter:setTeleportLocationRelativeToModuleSlot(Vector.new(-4350,-3750,1175))
        end,
	},
	[State.GAME_BEGIN] = {
        onEnter = function()
            GameTimer:pauseTimer()
	        GameTimer:setRoundTime(Gamemode:getNumberConfigVariable("MatchLengthSeconds") * 1000)
            GameTimer:resetTimer()
            GameTimer:startTimerWithCountdown(10000)
            bCanGameEndFromTimer = false;
            Promise.new(function () 
                LuauClock.timeout(15):await()
                bCanGameEndFromTimer = true;
            end )
            Gamemode:startGame()
            plinkoActive = true
            TrapSpinner:setCanMove(true)
            TrapSpinner2:setCanMove(true)
            TrapSpinner3:setCanMove(true)
            TrapSpinner4:setCanMove(true)
            TrapRotatingPlatform1:setCanMove(true)
            TrapRotatingPlatform2:setCanMove(true)
            TrapRotatingPlatform3:setCanMove(true)
            for i in Trap10SlidingPlatformArray do
                Trap10SlidingPlatformArray[i]:setCanMove(true)
            end
            trapDoorsActive = true
            ActivateTrap1()
            ActivateTrapPlinkoBalls()
            bWasGameRunning = true
        end,
	},
    [State.RUNNING] = {
        onEnter = function()
            runnerTeamSizeOnGameStart = RunnerTeamChanger:getTeamSize()
            killerTeamSizeOnGameStart = KillerTeamChanger:getTeamSize()
            TeamSwitcherBlocker:showLua()
            TeamSwitcherBlocker:setDefaultCollision()
            GameTimer:luaSetGameBegun(true)
            DisableForcefields()
        end,
    },
    [State.OVERTIME_COUNTDOWN] = {
        onEnter = function()
            -- log("enter overtime countdown")
            GameTimer:setRoundTime(0)
            GameTimer:resetTimer()
            GameTimer:pauseTimer()
            GameTimer:setRoundTime(0)
            GameTimer:startTimerWithCountdown(15000)
            GameTimer:setClockBounce(true)
            SpleefLockInBlocker:showLua()
            SpleefLockInBlocker:setDefaultCollision()
            SpectatorSpleefCheckpointBlocker:hideLua()
            SpectatorSpleefCheckpointBlocker:disableCollisionLua()
        end
	},
    [State.OVERTIME_RUNNING] = {
        onEnter = function()
            --log("enter overtime running")
            SpleefBlocker:hideLua()
            SpleefBlocker:disableCollisionLua()
            SpleefBlocker2:hideLua()
            SpleefBlocker2:disableCollisionLua()
            SpectatorSpleefCheckpointBlocker:showLua()
            SpectatorSpleefCheckpointBlocker:setDefaultCollision()
        end
	},
	[State.TEAM_SCORED_ENTER] = {
		onEnter = function()
            GameTimer:startCountdown(5000)
		end,
	},
	[State.TEAM_SCORED] = {
        onEnter = function()
		end,
		nextState = State.NEXT_POINT_COUNTDOWN,
        basicTimerLength = 5
	},	
    [State.PRACTICE_START] = {
        onEnter = function()
        end,
        nextState = State.PRACTICE_RUNNING,
        basicTimerLength = 1
	},		
    [State.PRACTICE_ON_SCORED] = {
        onEnter = function()
        end,
		nextState = State.PRACTICE_RUNNING,
        basicTimerLength = 1.5
	},	
    [State.PRACTICE_FLING_BALL] = {
        onEnter = function() 
        end,
		nextState = State.PRACTICE_RUNNING,
        basicTimerLength = 5
	},	
    [State.PRACTICE_RUNNING] = {
        onEnter = function() 
        end,
	},	
    [State.PRACTICE_ON_PASS_CAUGHT] = {
        onEnter = function() 
        end,
	},	
    [State.GAME_EXIT] = {
        onEnter = function()
            GameTimer:luaSetGameBegun(false)
            GameTimer:pauseTimer()
	        GameTimer:setRoundTime(Gamemode:getNumberConfigVariable("MatchLengthSeconds") * 1000)
            GameTimer:resetTimer()
            Gamemode:stopGame()
            plinkoActive = false
            EnableForcefields()
            ResetGameComponents()
            Promise.new(function () 
                ResetTriggerTeleporter:toggleRegularResetTeleporter()
                ResetTrigger:luaEnable()
                LuauClock.timeout(0.25):await()
                ResetTrigger:luaDisable()
            end )
            trapDoorsActive = false
            bCanGameEndFromTimer = false;
        end,
		nextState = State.BEGIN_PLAY,
        basicTimerLength = 10
	},  	
}

function SwitchState(state: number)
    GameStateManager:updateGameState(state)
    currentState = state
    stateMachineOnEnters[currentState].onEnter()
    local length = stateMachineOnEnters[currentState].basicTimerLength
    if length ~= nil then
        BasicTimer:start(length)
    end
end

function Debug_MoveBall() 
end

function BasicTimerFinished()
    if (stateMachineOnEnters[currentState].nextState) then
        SwitchState(stateMachineOnEnters[currentState].nextState)
    end
end

function OnTimerFinished(): ()
    if (currentState == State.OVERTIME_RUNNING or not bCanGameEndFromTimer) then
        return
	end
	GameTimer:pauseTimer()
    GameTimer:resetTimer()
    local goalInfo = GoalInfo.new({
        scoringTeam = 1,
        velocity = Vector.new(0, 0, 0),
        distance = 0,
        goalPosition = Vector.new(0,0,0),
        playerScored = "Scrappers",
        goalPoints = 1,
        bounces = 0,
        passes = 0,
        playerScoredTeam = 1,
        playerScoredID = -1,
        timestamp = 0,
    });
    GameScore:incrementScore(goalInfo)
    SwitchState(State.GAME_EXIT)
end

function CheckWhoHitEndTrigger(pawnID: number): ()
    local team = VRPawn.getTeamIndexByID(pawnID)
    if (team == 0 and currentState == State.RUNNING) then
        EndTrigger:disableCollisionLua()
        ResetTriggerTeleporter:toggleSpleefTeleporter()
        ResetTrigger:luaEnable()
        GameTimer:setRoundTime(0)
        SwitchState(State.OVERTIME_COUNTDOWN)
    end
end

function CheckpointLever2(): ()
    if (not Checkpoint2Active) then
        Checkpoint2Active = true
        CheckpointBlocker:hideLua()
        CheckpointBlocker:disableCollisionLua()
        Promise.new(function () 
            LuauClock.timeout(15):await()
            CheckpointBlocker:showLua()
            CheckpointBlocker:setDefaultCollision()
        end )
    end
end

function CheckpointLever1(): ()
    if (not Checkpoint1Active) then
        Checkpoint1Active = true
        CheckpointBlocker2:hideLua()
        CheckpointBlocker2:disableCollisionLua()
        Promise.new(function () 
            LuauClock.timeout(15):await()
            CheckpointBlocker2:showLua()
            CheckpointBlocker2:setDefaultCollision()
        end )
    end
end


function OnTimerCountdownFinished(): ()
    if (countdownFinishedDirections[currentState]) then
        SwitchState(countdownFinishedDirections[currentState])
    end
end

function PlayerJoinedTeam(): ()
end

function PlayerLeftTeam(teamSize: number, teamIndex: number): ()
    if (currentState == State.RUNNING or currentState == State.OVERTIME_RUNNING) then
       -- log("state was running or overtime running")
        if (teamSize == 0) then
           -- log("teamsize 0")
            if (teamIndex == 0) then
                local goalInfo = GoalInfo.new({
                    scoringTeam = 1,
	                velocity = Vector.new(0, 0, 0),
	                distance = 0,
	                goalPosition = Vector.new(0,0,0),
	                playerScored = "Scrappers",
	                goalPoints = 1,
	                bounces = 0,
	                passes = 0,
	                playerScoredTeam = 1,
                    playerScoredID = -1,
	                timestamp = 0,
                });
                GameScore:incrementScore(goalInfo)
            elseif (teamIndex == 1) then
                local goalInfo = GoalInfo.new({
                    scoringTeam = 0,
	                velocity = Vector.new(0, 0, 0),
	                distance = 0,
	                goalPosition = Vector.new(0,0,0),
	                playerScored = "Runners",
	                goalPoints = 1,
	                bounces = 0,
	                passes = 0,
	                playerScoredTeam = 0,
                    playerScoredID = -1,
	                timestamp = 0,
                });
                GameScore:incrementScore(goalInfo)
            end
            SwitchState(State.GAME_EXIT)
        end
    elseif (bWasGameRunning and (RunnerTeamChanger:getTeamSize() == 0) and (KillerTeamChanger:getTeamSize() == 0)) then
       -- log("state was NOT running or overtime running, resety game components")
        ResetGameComponents()
    end
end

function EnableForcefields(): ()
    Forcefield1:showLua()
    Forcefield1:setDefaultCollision()
    Forcefield2:showLua()
    Forcefield2:setDefaultCollision()
end
function DisableForcefields(): ()
    Forcefield1:hideLua()
    Forcefield1:disableCollisionLua()
end
function ResetGameComponents(): ()
    bWasGameRunning = false
    Trap1Button:luaEnableButton()
    Trap2Button:luaEnableButton()
    Trap2Button2:luaEnableButton()
    Trap4Button:luaEnableButton()
    Trap5Button:luaEnableButton()
    Trap6Button:luaEnableButton()
    Trap9Button:luaEnableButton()
    Trap9Button2:luaEnableButton()
    Trap11Button:luaEnableButton()
    KillerForcefield:hideLua()
    KillerForcefield:disableCollisionLua()
    TrapTrapDoor:luaDisable()
    TrapGrabHandles:luaDisable()
    TrapFan:luaDisable()
    TrapStickySlime:luaDisable()
    TrapStickySlime2:luaDisable()
    TrapBunnyHop1:luaDisable()
    TrapBunnyHop2:luaDisable()
    TrapBunnyHop3:luaDisable()
    TrapBunnyHop5:luaDisable()
    TrapBunnyHop6:luaDisable()
    TrapBunnyHop7:luaDisable()
    TrapBunnyHop8:luaDisable()
    TrapBunnyHop9:luaDisable()
    TrapBunnyHop10:luaDisable()
    TrapBunnyHop11:luaDisable()
    TrapBunnyHop12:luaDisable()
    TrapFallingBlock:luaDisable()
    TrapFallingBlock2:luaDisable()
    TrapFallingBlock3:luaDisable()
    TrapFallingBlock4:luaDisable()
    EndTrigger:setTriggerCollision()
    for i in Trap10SlidingPlatformArray do
        Trap10SlidingPlatformArray[i]:setIsBlack(false)
    end
    FreezeAndResetGiantDeathBall()
    FreezeAndResetPlinkoBalls()
    CheckpointBlocker:showLua()
    CheckpointBlocker:setDefaultCollision()
    CheckpointBlocker2:showLua()
    CheckpointBlocker2:setDefaultCollision()
    SpleefLockInBlocker:hideLua()
    SpleefLockInBlocker:disableCollisionLua()
    Checkpoint1Active = false
    Checkpoint2Active = false
    Checkpoint3Active = false
    CheckpointLightSwitch1:luaDisable()
    CheckpointLightSwitch2:luaDisable()
end

function ActivatePlinkoBalls(): ()
    for i in PlinkoBallsArray do
        PlinkoBallsArray[i]:spawnBall(false, Vector.new(0,0,0))
        PlinkoBallsArray[i]:getSpawnedBall():freeze(true)
        PlinkoBallsArray[i]:resetBall()
    end
end

function ActivateGiantDeathBall(): ()
    TrapDeathBall:setIfShouldFreezeBallUponReset(true)
    TrapDeathBall:spawnBall(false, Vector.new(0,0,0))
    TrapDeathBall:getSpawnedBall():freeze(true)
    TrapDeathBall:resetBall()
end

function ActivateTrapPlinkoBalls(): ()
        Promise.new(function () 
            plinkoActive = false
            for i in PlinkoBallsArray do
                local Ball = PlinkoBallsArray[i]:getSpawnedBall()
                if (Ball ~= nil) then
                    PlinkoBallsArray[i]:resetBall()
                    Ball:unFreeze(true)
                end
            end
        end )
end

function FreezeAndResetPlinkoBalls(): ()
    for i in PlinkoBallsArray do
        local Ball = PlinkoBallsArray[i]:getSpawnedBall()
        if (Ball ~= nil) then
            Ball:freeze(true)
            PlinkoBallsArray[i]:resetBall()
        end
    end
end

function FreezeAndResetGiantDeathBall(): ()
    local Ball = TrapDeathBall:getSpawnedBall()
    if (Ball ~= nil) then
        Ball:freeze(true)
        TrapDeathBall:resetBall()
    end
end

function ActivateTrap1(): ()
    Promise.new(function () 
        if (trapDoorsActive) then
            local i = math.random(1, #Trap1Array)
            Trap1Array[i]:luaEnable()
            LuauClock.timeout(2.25):await()
            ActivateTrap1()
        end
    end )
end

function ActivateTrap1FallingBlocks(): ()
    TrapFallingBlock5:luaEnable()
    TrapFallingBlock6:luaEnable()
end

function ActivateTrap2(): ()
    TrapSpinner:setRevolutionsPerCycle(4)
    TrapSpinner2:setRevolutionsPerCycle(4)
    TrapSpinner3:setRevolutionsPerCycle(4)
    TrapSpinner4:setRevolutionsPerCycle(4)
end

function ReverseTrap2(): ()
    TrapSpinner:reverseDirection()
    TrapSpinner2:reverseDirection()
    TrapSpinner3:reverseDirection()
    TrapSpinner4:reverseDirection()
end

function ActivateTrap3(): ()
    Promise.new(function () 
        local RandomizedBlockArray = table.clone(Trap3FallingBlockArray)
        RandomizedBlockArray = shuffle(RandomizedBlockArray)
        for i in RandomizedBlockArray do
            RandomizedBlockArray[i]:luaEnable()
            LuauClock.timeout(0.5):await()
        end
        table.clear(RandomizedBlockArray)
    end )
end

function shuffle(tbl)
    for i = #tbl, 2, -1 do
      local j = math.random(i)
      tbl[i], tbl[j] = tbl[j], tbl[i]
    end
    return tbl
end


function ActivateTrap4(): ()
    TrapFan:luaEnable()
end

function ActivateTrap5(): ()
    TrapStickySlime:luaEnable()
    TrapStickySlime2:luaEnable()
end

function ActivateTrap6(): ()
    Promise.new(function () 
        for i=#Trap7BunnyhopArray, 1, -2 do
            Trap7BunnyhopArray[i]:luaEnable()
            Trap7BunnyhopArray[i-1]:luaEnable()
            LuauClock.timeout(0.5):await()
        end
    end )
end

function ActivateTrap9(): ()
    TrapRotatingPlatform1:setRevolutionsPerCycle(2)
    TrapRotatingPlatform2:setRevolutionsPerCycle(2)
    TrapRotatingPlatform3:setRevolutionsPerCycle(2)
end

function ReverseTrap9(): ()
    TrapRotatingPlatform1:reverseDirection()
    TrapRotatingPlatform2:reverseDirection()
    TrapRotatingPlatform3:reverseDirection()
end

function ActivateTrap10(): ()
    Promise.new(function () 
        for i in Trap10SlidingPlatformArray do
            Trap10SlidingPlatformArray[i]:setIsBlack(true)
        end
        LuauClock.timeout(2):await()
        for i in Trap10SlidingPlatformArray do
            Trap10SlidingPlatformArray[i]:setIsBlack(false)
        end
    end )
end

function ActivateTrap11(): ()
    local Ball = TrapDeathBall:getSpawnedBall()
    TrapDeathBall:resetBall(false, Vector.new(0,0,0))
    Ball:unFreeze(true)
    Ball:setVelocity(TrapDeathBall:getForwardVectorLuau() * -2600)
end


SecretSwitch1 = false
SecretSwitch2 = false
SecretSwitch3 = false
function CheckSecretState(): ()
    if (SecretSwitch1 and SecretSwitch2 and SecretSwitch3) then
        SecretDoor:hideLua()
        SecretDoor:disableCollisionLua()
    else
        SecretDoor:showLua()
        SecretDoor:setDefaultCollision()
    end
end

function ActivateSecret1(): ()
    SecretSwitch1 = true
    CheckSecretState()
end
function ActivateSecret2(): ()
    SecretSwitch2 = true
    CheckSecretState()
end
function ActivateSecret3(): ()
    SecretSwitch3 = true
    CheckSecretState()
end
function DeActivateSecret1(): ()
    SecretSwitch1 = false
    CheckSecretState()
end
function DeActivateSecret2(): ()
    SecretSwitch2 = false
    CheckSecretState()
end
function DeActivateSecret3(): ()
    SecretSwitch3 = false
    CheckSecretState()
end

ScraprunOpen = false

function ScraprunNetvarsChanged(): ()
    if (Gamemode:getBoolConfigVariable("bScraprunOpen")) then
        ScraprunOpen = true
        ScraprunBlocker:hideLua()
        ScraprunBlocker:disableCollisionLua()
    else
        ScraprunOpen = false
        ScraprunBlocker:showLua()
        ScraprunBlocker:setDefaultCollision()
    end
end

function BeginPlay()
	BasicTimer.finished.Listen(BasicTimerFinished)
	GameTimer.onCountdownEnd.Listen(OnTimerCountdownFinished)
	GameTimer.onTimerStopped.Listen(OnTimerFinished)
	GameTimer.enterGameState.Listen(SwitchState)
	KillerTeamChanger.teamSizeDecreased.Listen(PlayerLeftTeam)
	RunnerTeamChanger.teamSizeDecreased.Listen(PlayerLeftTeam)
    StartButton.onButtonPressEvent.Listen(OnStartGameButtonPress)
    Trap1Button.onButtonPressEvent.Listen(ActivateTrap1FallingBlocks)
    Trap2Button.onButtonPressEvent.Listen(ActivateTrap2)
    Trap2Button2.onButtonPressEvent.Listen(ReverseTrap2)
    Trap3Button.onButtonPressEvent.Listen(ActivateTrap3)
    Trap4Button.onButtonPressEvent.Listen(ActivateTrap4)
    Trap5Button.onButtonPressEvent.Listen(ActivateTrap5)
    Trap6Button.onButtonPressEvent.Listen(ActivateTrap6)
    Trap9Button.onButtonPressEvent.Listen(ActivateTrap9)
    Trap9Button2.onButtonPressEvent.Listen(ReverseTrap9)
    Trap10Button.onButtonPressEvent.Listen(ActivateTrap10)
    Trap11Button.onButtonPressEvent.Listen(ActivateTrap11)
    PlinkoStartButton.onButtonPressEvent.Listen(ActivateTrapPlinkoBalls)
    EndTrigger.onOverlapByPlayerServer.Listen(CheckWhoHitEndTrigger)
    CheckpointLightSwitch2.blueprintOnEnabled.Listen(CheckpointLever2)
    CheckpointLightSwitch1.blueprintOnEnabled.Listen(CheckpointLever1)
    KillerlessRunLightSwitch.blueprintOnEnabled.Listen(OnStartGameButtonPress)
    SecretLightSwitch1.blueprintOnEnabled.Listen(ActivateSecret1)
    SecretLightSwitch2.blueprintOnEnabled.Listen(ActivateSecret2)
    SecretLightSwitch3.blueprintOnEnabled.Listen(ActivateSecret3)
    SecretLightSwitch1.blueprintOnDisabled.Listen(DeActivateSecret1)
    SecretLightSwitch2.blueprintOnDisabled.Listen(DeActivateSecret2)
    SecretLightSwitch3.blueprintOnDisabled.Listen(DeActivateSecret3)
    ScraprunNetvarsChanged()
    Gamemode.onConfigChanged.Listen(ScraprunNetvarsChanged)
    table.insert(Trap3FallingBlockArray, TrapFallingBlock)
    table.insert(Trap3FallingBlockArray, TrapFallingBlock2)
    table.insert(Trap3FallingBlockArray, TrapFallingBlock3)
    table.insert(Trap3FallingBlockArray, TrapFallingBlock4)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop1)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop2)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop3)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop4)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop5)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop6)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop7)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop8)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop9)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop10)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop11)
    table.insert(Trap7BunnyhopArray, TrapBunnyHop12)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform1)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform2)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform3)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform4)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform5)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform6)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform7)
    table.insert(Trap10SlidingPlatformArray, TrapSlidingPlatform8)
    table.insert(Trap1Array, TrapTrapDoor)
    table.insert(Trap1Array, TrapTrapDoor2)
    table.insert(Trap1Array, TrapTrapDoor3)
    table.insert(PlinkoBallsArray, PlinkoBall1)
    table.insert(PlinkoBallsArray, PlinkoBall2)
    table.insert(PlinkoBallsArray, PlinkoBall3)
    table.insert(PlinkoBallsArray, PlinkoBall4)
    table.insert(PlinkoBallsArray, PlinkoBall5)
    table.insert(PlinkoBallsArray, PlinkoBall6)
    ActivateGiantDeathBall()
    ActivatePlinkoBalls()
    GameTimer:resetTimer()
    GameTimer:pauseTimer()
    GameTimer:setRoundTime(Gamemode:getNumberConfigVariable("MatchLengthSeconds") * 1000)
    EnableForcefields()
    ResetTrigger:luaDisable()
    SwitchState(State.WAITING_TO_START)
end

function EndPlay()
	table.clear(Trap3FallingBlockArray)
    table.clear(Trap7BunnyhopArray)
    table.clear(Trap10SlidingPlatformArray)
end