GoalieTrainingLocationsArray = {
	Vector.new(2.998538, 119.090794, 207.935547),
	Vector.new(-281.232418, 175.940410, 207.935547),
	Vector.new(-426.630364, 289.948226, 207.935547),
	Vector.new(-574.910145, 478.843127, 207.935547),
	Vector.new(281.862065, 176.994229, 207.935547),
	Vector.new(427.270759, 290.490197, 207.935547),
	Vector.new(576.825207, 479.800985, 207.935547)
}

ShootingTrainingLocationsArray = {
	Vector.new(551.825222, 1104.800985, 157.935547),
	Vector.new(-550.999973, 1104.801012, 157.935547),
	Vector.new(-648.174793, 479.801015, 132.935547),
	Vector.new(651.825207, 479.800983, 132.935547),
	Vector.new(551.825220, 1025.999960, 81.560547),
	Vector.new(-551.825170, 1025.999987, 81.560547),
	Vector.new(576.825207, 479.800985, 207.935547)
}

ShootingTrainingRotationArray = {
	Vector.new(-0, 120, 0),
	Vector.new(-0, -60, 0),
	Vector.new(-0, -60, 0),
	Vector.new(30, 180, 0),
	Vector.new(-30, 180, 0),
	Vector.new(-0, -60, 0),
	Vector.new(-0, 120, 0)
}

local TrainingEvent: EventComponent = nil
local GoalieTrainingComponent: GoalieTrainingComponent = nil
local ShootingTrainingComponent: ShootingTrainingComponent = nil
local TeamChanger: TeamChangeComponent = nil

bGoalieTrainingEnabled = false
bShootingTrainingEnabled = false

function BeginPlay()
	GoalieTrainingComponent.BlueprintOnEnabled.Listen(EnableGoalieTraining)
	GoalieTrainingComponent.BlueprintOnDisabled.Listen(DisableGoalieTraining)
	GoalieTrainingComponent.SpawnNextBall.Listen(HandleSpawningGoalieBall)
	ShootingTrainingComponent.BlueprintOnEnabled.Listen(EnableShootingTraining)
	ShootingTrainingComponent.BlueprintOnDisabled.Listen(DisableShootingTraining)
	ShootingTrainingComponent.SpawnNextBall.Listen(HandleSpawningShootingBall)
	--TrainingEvent.ServerEvent.Listen(HandleSpawningBall)
	TeamChanger.TeamSizeIncreased.Listen(TeamJoined)
	TeamChanger.TeamSizeDecreased.Listen(TeamLeft)
end

function EnableGoalieTraining()
	bGoalieTrainingEnabled = true
	bShootingTrainingEnabled = false
end

function DisableGoalieTraining()
	bGoalieTrainingEnabled = false
end

function EnableShootingTraining()
	bShootingTrainingEnabled = true
	bGoalieTrainingEnabled = false
	--log("in EnableShootingTraining")
end

function DisableShootingTraining()
	bShootingTrainingEnabled = false
	--log("in DisableShootingTraining")
end

function EndPlay()
end

function HandleSpawningShootingBall()
	local RandomIndex = math.random(1, #ShootingTrainingLocationsArray)
	local Location = ShootingTrainingLocationsArray[RandomIndex]
	local Rotation = ShootingTrainingRotationArray[RandomIndex]
	local Ball = ShootingTrainingComponent:requestTrainingBallAtLocation(Location, Rotation)
	TrainingEvent:eventCompleteWithDiscArgument(Ball)
end

function HandleSpawningGoalieBall()
	local RandomIndex = math.random(1, #GoalieTrainingLocationsArray)
	local Location = GoalieTrainingLocationsArray[RandomIndex]
	local Rotation = Vector.new(0,0,0)
	local Ball = GoalieTrainingComponent:requestTrainingBallAtLocation(Location, Rotation)
	TrainingEvent:eventCompleteWithDiscArgument(Ball)
end

function TeamJoined()
	ShootingTrainingComponent:playerHasJoined()
	GoalieTrainingComponent:playerHasJoined()
end

function TeamLeft()
	if (bGoalieTrainingEnabled) then
		GoalieTrainingComponent:endBecausePlayerLeft()
	elseif (bShootingTrainingEnabled) then
		ShootingTrainingComponent:endBecausePlayerLeft()
	end
end