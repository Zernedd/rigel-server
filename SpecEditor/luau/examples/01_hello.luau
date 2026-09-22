--!strict
-- 01_hello.luau
-- The smallest useful script. Attach it to any sandbox object.
-- Every machine that has the object (the server, the editor, every player) runs it,
-- so you will see the line once in EACH machine's log:
--     LogLuau: Display: [01_hello.luau]: Hello from player 3 -- 20 + 22 = 42

-- Code at the top level runs as soon as the script is loaded.
local answer = 20 + 22

-- BeginPlay is called by the game once the script's object is ready.
function BeginPlay()
	-- VRPawn.getLocalPlayerIndex() is the index of the player on THIS machine.
	-- (On the dedicated server there is no local player, so expect a placeholder value there.)
	local me = VRPawn.getLocalPlayerIndex()
	log("Hello from player " .. tostring(me) .. " -- 20 + 22 = " .. tostring(answer))
end

-- EndPlay is called when the object or its gamemode is torn down (for example when the
-- object is rebuilt because you attached a new version of this script).
function EndPlay()
	log("Goodbye")
end
