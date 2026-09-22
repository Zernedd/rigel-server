--!strict
-- External Dependencies


local MainEventRepeater: EventComponent = nil
local DataTable: DataTableComponent = nil

local Cup_1: GolfCupComponent = nil
local Cup_2: GolfCupComponent = nil
local Cup_3: GolfCupComponent = nil
local Cup_4: GolfCupComponent = nil
local Cup_5: GolfCupComponent = nil
local Cup_6: GolfCupComponent = nil
local Cup_7: GolfCupComponent = nil
local Cup_8: GolfCupComponent = nil
local Cup_9: GolfCupComponent = nil

local Tee_1: BallSpawnerComponent = nil
local Tee_2: BallSpawnerComponent = nil
local Tee_3: BallSpawnerComponent = nil
local Tee_4: BallSpawnerComponent = nil
local Tee_5: BallSpawnerComponent = nil
local Tee_6: BallSpawnerComponent = nil
local Tee_7: BallSpawnerComponent = nil
local Tee_8: BallSpawnerComponent = nil
local Tee_9: BallSpawnerComponent = nil


local TeeActual_1: GolfTeeComponent = nil
local TeeActual_2: GolfTeeComponent = nil
local TeeActual_3: GolfTeeComponent = nil
local TeeActual_4: GolfTeeComponent = nil
local TeeActual_5: GolfTeeComponent = nil
local TeeActual_6: GolfTeeComponent = nil
local TeeActual_7: GolfTeeComponent = nil
local TeeActual_8: GolfTeeComponent = nil
local TeeActual_9: GolfTeeComponent = nil


local Hole_1: PhysicalComponent = nil
local Hole_2: PhysicalComponent = nil
local Hole_3: PhysicalComponent = nil
local Hole_4: PhysicalComponent = nil
local Hole_5: PhysicalComponent = nil
local Hole_6: PhysicalComponent = nil
local Hole_7: PhysicalComponent = nil
local Hole_8: PhysicalComponent = nil
local Hole_9: PhysicalComponent = nil


local HoleDribble_1: PhysicalComponent = nil
local HoleDribble_2: PhysicalComponent = nil
local HoleDribble_3: PhysicalComponent = nil
local HoleDribble_4: PhysicalComponent = nil
local HoleDribble_5: PhysicalComponent = nil
local HoleDribble_6: PhysicalComponent = nil
local HoleDribble_7: PhysicalComponent = nil
local HoleDribble_8: PhysicalComponent = nil
local HoleDribble_9: PhysicalComponent = nil


-- End External Dependencies

local CurrentHoleIndex = 0
local MyBall: DiscEntity = nil

local Cups: { GolfCupComponent }
local Tees: { BallSpawnerComponent }
local TeeActual: { GolfTeeComponent }
local Holes: { PhysicalComponent }
local DribbleHoles: { PhysicalComponent }

-- A map from Tees to the balls they spawned
local SpawnedBalls: { [BallSpawnerComponent]: DiscEntity? } = {}



local IsDribble = false
local ChangingModes = false;
local Dribble_StartTime = 0

function DisableHole(HoleNumber: number, DestroyBall: boolean?)
	--log("Disabling hole " .. HoleNumber)
	--log("Holes: " .. #Holes)
	--log("Cups: " .. #Cups)
	local Hole = Holes[HoleNumber]
	local Cup = Cups[HoleNumber]
	local Tee = Tees[HoleNumber]
	if (Cup ~= nil) then
		Cup:setCupVisibility(false)
		Cup:setOutlineVisibility(false)
	end
	local Ball = SpawnedBalls[Tee]
	if Ball then
		if DestroyBall then
			Tee:destroyBall(Ball)
		end
	end
	SpawnedBalls[Tee] = nil

end

function EnableHole(HoleNumber: number)
	--log("Enabling hole " .. HoleNumber)
	local Hole = Holes[HoleNumber]
	local Cup = Cups[HoleNumber]
	local Tee = Tees[HoleNumber]
	local Actual = TeeActual[HoleNumber]

	Cup:setCupVisibility(true)
	Cup:setOutlineVisibility(true)

	local Test: Ref = {}

	if ChangingModes then
		--log("Changing Modes")
		ChangingModes = false
		Tee:destroyBall(MyBall)
		LuauClock.timeout(0):andThen(function()
			EnableHole(HoleNumber)
		end)
		return
	end


	local BallType = ""
	if IsDribble then
		BallType = "Dribble"
	end
	local Parameters = BallSpawnParameters.new({
		position = Vector.new(1, 2, 3),
		isPersonalBall = true,
		targetPlayerIndex = VRPawn.getLocalPlayerIndex(),
		type = BallType,
	});
	--log("Spawning Ball")


	Tee:spawnBallWithParameters(Parameters):andThen(function(Disc: DiscEntity)
		--log("Ball Now Spawned On Client")
		-- Disc:SetEnableHUDIndicator2(true)
		Disc:setEnableHUDIndicator(true)
		SpawnedBalls[Tee] = Disc
		MyBall = Disc

		-- When the ball goes to sleep/rest. Lift it up 10 units slowly
		Disc.onGoToSleep.Listen(function()
			--log("onGoToSleep")
			Disc:addSmoothOffset(Disc:getGravityDirection() * -10)
		end)
	end)
end

-- local Style = CellStyle.New({
-- 	TextJustification = Enum.TextJustify.Center,
-- })

-- function BallSPawned(Disc: DiscEntity)
-- end
-- function EnableHole(HoleNumber: number)
-- 	--log("Enabling hole " .. HoleNumber)
-- 	local Hole = Holes[HoleNumber]
-- 	local Cup = Cups[HoleNumber]
-- 	local Tee = Tees[HoleNumber]
-- 	Cup:setCupVisibility(true)

-- 	task.spawn()
-- 	coroutine.running()
-- 	local Parameters = BallSpawnParameters.New({
-- 		Position = Vector.New(1, 2, 3),
-- 		IsPersonalBall = true,
-- 	});

-- 	Tee:spawnBallWithParameters(Parameters).Then(BallSPawned);
-- end


-- function EnableHole(HoleNumber: number)
-- 	--log("Enabling hole " .. HoleNumber)
-- 	local Hole = Holes[HoleNumber]
-- 	local Cup = Cups[HoleNumber]
-- 	local Tee = Tees[HoleNumber]
-- 	Cup:setCupVisibility(true)

-- 	local Parameters = BallSpawnParameters.New({
-- 		Position = Vector.New(1, 2, 3),
-- 		IsPersonalBall = true,
-- 	});

-- 	Tee:spawnBallWithParameters(Parameters).Then(function(Disc: DiscEntity) 
-- 		--log("Ball Now Spawned On Client")
-- 		-- Disc:SetEnableHUDIndicator2(true)
-- 		Disc:setEnableHUDIndicator(true)
-- 		SpawnedBalls[Tee] = Disc
-- 		MyBall = Disc
-- 	end);
-- end



local EarlyOut = function() end;

function SwitchToHole(HoleNumber: number)
	EarlyOut()
	--log("Switching to hole " .. HoleNumber)
	if CurrentHoleIndex > 0 and CurrentHoleIndex <= #Holes then
		DisableHole(CurrentHoleIndex)
	end
	EnableHole(HoleNumber)
	CurrentHoleIndex = HoleNumber
end

function OnBallInCup(Cup: GolfCupComponent, Score: number, Disc: DiscEntity)
	--log("Ball in cup event")
	if Disc ~= MyBall then
		--log("Ball in cup event for a ball that isn't mine")
		return
	end

	if IsDribble then
		local TimeMS = (LuauClock.getTime() - Dribble_StartTime) * 1000
		-- --log("Dribble")
		Quests.eventNumber(-1, "TKB_Golf_DribbleBallInCup" , TimeMS)
		Quests.eventNumber(-1, "TKB_Golf_DribbleBallInCup_" .. CurrentHoleIndex, TimeMS)
	else
		-- --log("Not Dribble")
		Quests.eventNumber(-1, "TKB_Golf_BallInCup", Score)
		Quests.eventNumber(-1, "TKB_Golf_BallInCup_" .. CurrentHoleIndex, Score)
	end

	MainEventRepeater:triggerEventWithData({
		EventType = "BallInCup",
		Cup = CurrentHoleIndex,
		Score = Score,
		Dribble = IsDribble;
		Time = LuauClock.getTime() - Dribble_StartTime,
		PlayerID = VRPawn.getLocalPlayerIndex(),
	})

	Promise.new(function () 
		-- Do something Now
		LuauClock.timeout(5):await()
		-- Do something after 5 seconds
		LuauClock.timeout(5):await()
		-- Do something after 10 seconds
	end)

			

	local NextTee = TeeActual[CurrentHoleIndex + 1]

	if NextTee then 
	Cup:setEnableArrowIndicator(true)
		NextTee:setEnableArrow(true)


	
		local Timeout = LuauClock.timeout(45)
		local EarlyOutTest = Promise.new(function (Resolve:(number) -> (), Reject:(any) -> ())
			EarlyOut = function()
				Resolve(1)
			end
		end)
		Promise.race({
			Timeout,
			EarlyOutTest
		}):andThen(function()
			EarlyOut = function() end
		Cup:setEnableArrowIndicator(false)
			NextTee:setEnableArrow(false)
	end)
	end




	local NextHoleIndex = CurrentHoleIndex + 1

	if NextHoleIndex > 0 and NextHoleIndex <= #Holes then
		-- SwitchToHole(NextHoleIndex)
		DisableHole(CurrentHoleIndex, true)
	else
		DisableHole(CurrentHoleIndex, true)
		CurrentHoleIndex = -1
	end

	
end

function BeginPlay()

	
	-- TeeActual_1:setEnableArrow(true)
	-- LuauClock.timeout(3):andThen(function()
	-- 	TeeActual_1:setEnableArrow(false)
	-- end)
	-- Cup_1.onBallInCup_Multicast.Listen(OnBallInCup)
	-- Promise.new(function (Resolve:(string) -> (), Reject:(number) -> ())
	-- 	Cup_1.onBallInCup_Multicast.Listen(OnBallInCup)
	-- end)
	-- -- --log("Doing Stuff")
	-- -- --log(Promise)
	-- local Promise2 = Promise.new(function (Resolve:(string) -> (), Reject:(number) -> ())
	-- 	Resolve("Hello")
	-- end):andThen(function (Value: string)
	-- 	--log("Inside of the andThen")
	-- 	--log(Value)
	-- 	return nil
	-- end)

	-- --log("Promise2")
	-- --log(Promise.resolve("Hello"))

	-- local Promise2 = Promise.resolve("Hello"):andThen(function (Value: string)
	-- 	--log("Inside of the andThen 2")
	-- 	--log(Value)
	-- 	return nil
	-- end):expect()

	-- --log(Promise.try(function()
	-- 	return math.random(1, 2) == 1 and "ok" or error("Oh an error!")
	-- end):andThen(function (Value: string)
	-- 	--log("Inside of the andThen")
	-- 	--log(Value)
	-- 	-- return nil
	-- end))

	-- --log(Promise.try(function()
	-- 	return math.random(1, 2) == 1 and "ok" or error("Oh an error!")
	-- end))
	-- --log(Promise.try(function()
	-- 	--log(math)
	-- 	-- return math.random(1, 200) -- == 1 and "ok" or error("Oh an error!")
	-- 	return "ok"
	-- end):expect())
	-- --log(math)

	
	local Font = CellStyle.new({
		TextJustification = Enum.TextJustify.Center,
	})
	--log(Font)
	Cups = { 
		Cup_1, 
		Cup_2, 
		Cup_3,
		Cup_4,
		Cup_5,
		Cup_6,
		Cup_7,
		Cup_8,
		Cup_9
	}
	Tees = { 
		Tee_1,
		Tee_2,
		Tee_3,
		Tee_4,
		Tee_5,
		Tee_6,
		Tee_7,
		Tee_8,
		Tee_9
	}
	TeeActual = {
		TeeActual_1,
		TeeActual_2,
		TeeActual_3,
		TeeActual_4,
		TeeActual_5,
		TeeActual_6,
		TeeActual_7,
		TeeActual_8,
		TeeActual_9
	}


	Holes = { 
		Hole_1,
		Hole_2,
		Hole_3,
		Hole_4,
		Hole_5,
		Hole_6,
		Hole_7,
		Hole_8,
		Hole_9
	}

	DribbleHoles = { 
		HoleDribble_1,
		HoleDribble_2,
		HoleDribble_3,
		HoleDribble_4,
		HoleDribble_5,
		HoleDribble_6,
		HoleDribble_7,
		HoleDribble_8,
		HoleDribble_9
	}

	for i, hole in ipairs(Cups) do
		DisableHole(i)
		hole.onBallInCup_Multicast.Listen(function(Cup: GolfCupComponent, Score: number, Disc: DiscEntity) 
			if Disc == MyBall then
				if CurrentHoleIndex == i then
					OnBallInCup(Cup, Score, Disc)
				else 
					DisableHole(CurrentHoleIndex, true)
				end
			end
		end
		)
	end

	for i, hole in ipairs(TeeActual) do
		hole.onManualStart.Listen(function()
			--log("Manual Start")
			Quests.event(-1, "TKB_Golf_HoleStarted")
			Quests.event(-1, "TKB_Golf_HoleStarted_" .. i)
			--log(`Player ${VRPawn.getLocalPlayerIndex()} started hole ${i}!`)

			if IsDribble and CurrentHoleIndex == i then
				ChangingModes = true
			end

			IsDribble = false
			MainEventRepeater:triggerEventWithData({
				EventType = "OnPlayerStartHole",
				HoleIndex = i,
				PlayerID = VRPawn.getLocalPlayerIndex(),
			})
			SwitchToHole(i)
		end)
		hole.onManualReset.Listen(function()  
			--log("Manual Reset")
			DisableHole(CurrentHoleIndex, true)
		end
		)
	end

	for i, hole in ipairs(Holes) do
		hole.onOverlapByPlayerClientside.Listen(function()
			Quests.event(-1, "TKB_Golf_HoleStarted")
			Quests.event(-1, "TKB_Golf_HoleStarted_" .. i)

			--log(`Player ${VRPawn.getLocalPlayerIndex()} started hole ${i}!`)
		
			if IsDribble and CurrentHoleIndex == i then
				ChangingModes = true
			end

			IsDribble = false
			MainEventRepeater:triggerEventWithData({
				EventType = "OnPlayerStartHole",
				HoleIndex = i,
				PlayerID = VRPawn.getLocalPlayerIndex(),
			})
			SwitchToHole(i)
		end)
	end

	for i, hole in ipairs(DribbleHoles) do
		hole.onOverlapByPlayerClientside.Listen(function()
			Quests.event(-1, "TKB_Golf_DribbleHoleStarted")
			Quests.event(-1, "TKB_Golf_DribbleHoleStarted_" .. i)
			
			--log(`Player ${VRPawn.getLocalPlayerIndex()} started dribble hole ${i}!`)

			if not IsDribble and CurrentHoleIndex == i then
				ChangingModes = true
			end

			IsDribble = true
			Dribble_StartTime = LuauClock.getTime()
			MainEventRepeater:triggerEventWithData({
				EventType = "OnPlayerStartHole",
				HoleIndex = i,
				PlayerID = VRPawn.getLocalPlayerIndex(),
			})
			SwitchToHole(i)
		end)
	end
end