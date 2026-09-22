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

local ActualTee_1: GolfTeeComponent = nil
local ActualTee_2: GolfTeeComponent = nil
local ActualTee_3: GolfTeeComponent = nil
local ActualTee_4: GolfTeeComponent = nil
local ActualTee_5: GolfTeeComponent = nil
local ActualTee_6: GolfTeeComponent = nil
local ActualTee_7: GolfTeeComponent = nil
local ActualTee_8: GolfTeeComponent = nil
local ActualTee_9: GolfTeeComponent = nil

local Hole_1: PhysicalComponent = nil
local Hole_2: PhysicalComponent = nil
local Hole_3: PhysicalComponent = nil
local Hole_4: PhysicalComponent = nil
local Hole_5: PhysicalComponent = nil
local Hole_6: PhysicalComponent = nil
local Hole_7: PhysicalComponent = nil
local Hole_8: PhysicalComponent = nil
local Hole_9: PhysicalComponent = nil

-- End External Dependencies

local CurrentHoleIndex = 0
local MyBall: DiscEntity = nil

local Cups: { GolfCupComponent }
local Tees: { BallSpawnerComponent }
local Holes: { PhysicalComponent }
local ActualTees: { GolfTeeComponent}

-- A map from Tees to the balls they spawned
local SpawnedBalls: { [BallSpawnerComponent]: DiscEntity } = {}

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
end

function EnableHole(HoleNumber: number)
	--log("Enabling hole " .. HoleNumber)
	local Hole = Holes[HoleNumber]
	local Cup = Cups[HoleNumber]
	local Tee = Tees[HoleNumber]
	Cup:setCupVisibility(true)
	Cup:setOutlineVisibility(true)

	local Test: Ref = {}


	local Parameters = BallSpawnParameters.new({
		position = Vector.new(1, 2, 3),
		isPersonalBall = true,
		targetPlayerIndex = VRPawn.getLocalPlayerIndex(),
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
-- 	Cup:SetCupVisibility(true)

-- 	task.spawn()
-- 	coroutine.running()
-- 	local Parameters = BallSpawnParameters.New({
-- 		Position = Vector.New(1, 2, 3),
-- 		IsPersonalBall = true,
-- 	});

-- 	Tee:SpawnBallWithParameters(Parameters).Then(BallSPawned);
-- end


-- function EnableHole(HoleNumber: number)
-- 	--log("Enabling hole " .. HoleNumber)
-- 	local Hole = Holes[HoleNumber]
-- 	local Cup = Cups[HoleNumber]
-- 	local Tee = Tees[HoleNumber]
-- 	Cup:SetCupVisibility(true)

-- 	local Parameters = BallSpawnParameters.New({
-- 		Position = Vector.New(1, 2, 3),
-- 		IsPersonalBall = true,
-- 	});

-- 	Tee:SpawnBallWithParameters(Parameters).Then(function(Disc: DiscEntity) 
-- 		--log("Ball Now Spawned On Client")
-- 		-- Disc:SetEnableHUDIndicator2(true)
-- 		Disc:SetEnableHUDIndicator(true)
-- 		SpawnedBalls[Tee] = Disc
-- 		MyBall = Disc
-- 	end);
-- end


function SwitchToHole(HoleNumber: number)
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

	Quests.eventNumber(-1, "CLB_Golf_BallInCup", Score)
	Quests.eventNumber(-1, "CLB_Golf_BallInCup_" .. CurrentHoleIndex, Score)

	MainEventRepeater:triggerEventWithData({
		EventType = "BallInCup",
		Cup = CurrentHoleIndex,
		Score = Score,
		PlayerID = VRPawn.getLocalPlayerIndex(),
	})

			
	Cup:setEnableArrowIndicator(true)
	LuauClock.timeout(5):andThen(function()
		Cup:setEnableArrowIndicator(false)
	end)

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

	ActualTees = {
		ActualTee_1,
		ActualTee_2,
		ActualTee_3,
		ActualTee_4,
		ActualTee_5,
		ActualTee_6,
		ActualTee_7,
		ActualTee_8,
		ActualTee_9
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

	for i, hole in ipairs(ActualTees) do
		hole.onManualStart.Listen(function()
			--log("Manual Start")
			--log(`Player ${VRPawn.getLocalPlayerIndex()} started hole ${i}!`)

			Quests.event(-1, "CLB_Golf_HoleStarted")
			Quests.event(-1, "CLB_Golf_HoleStarted_" .. i)

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

			Quests.event(-1, "CLB_Golf_HoleStarted")
			Quests.event(-1, "CLB_Golf_HoleStarted_" .. i)

			--log(`Player ${VRPawn.getLocalPlayerIndex()} started hole ${i}!`)
			MainEventRepeater:triggerEventWithData({
				EventType = "OnPlayerStartHole",
				HoleIndex = i,
				PlayerID = VRPawn.getLocalPlayerIndex(),
			})
			SwitchToHole(i)
		end)
	end
end