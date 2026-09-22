--!strict
-- 06_vectors_and_balls.luau
-- Vector maths plus a ball spawner: spawn a personal ball for the local player, then
-- launch it straight up whenever it comes to rest. Mirrors TKBGolf's Course.luau.
--
-- `Spawner` comes from the object's references wiring (not configurable in the editor yet).

-- External Dependencies
local Spawner: BallSpawnerComponent = nil
-- End External Dependencies

-- Vectors support + - * / and have .x .y .z
local function length(v: Vector): number
	return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
end

local function launch(ball: DiscEntity)
	local up = ball:getGravityDirection() * -1    -- opposite of gravity
	ball:setVelocity(up * 1500)                   -- Unreal units: cm/s
	log(`launched; speed now {math.floor(length(ball:getDiscVelocity()))}`)
end

function BeginPlay()
	if (Spawner :: BallSpawnerComponent?) == nil then
		log("Spawner reference is not wired")
		return
	end

	local params = BallSpawnParameters.new({
		position = Spawner:getActorLocationLuau() + Vector.new(0, 0, 100),
		isPersonalBall = true,
		targetPlayerIndex = VRPawn.getLocalPlayerIndex(),
	})

	-- spawnBallWithParameters returns a promise that resolves with the ball on this machine.
	Spawner:spawnBallWithParameters(params):andThen(function(ball: DiscEntity)
		ball:setEnableHUDIndicator(true)
		ball.onGoToSleep.Listen(function()
			launch(ball)
		end)
	end)
end
