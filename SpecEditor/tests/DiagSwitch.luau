--!strict
-- TEST FIXTURE for diag_*.txt (not part of the kit): which visibility / toggle calls take effect, and where.
-- Copy it into Documents\RigelScripts before running a diag test, and remove it afterwards.
local Switch: ToggleableComponent = nil
local Target: PhysicalComponent = nil

local function t(): number return math.floor(LuauClock.getTime() * 10 + 0.5) / 10 end

local steps = {
	{ 3, "hideLua", function() Target:hideLua() end },
	{ 3, "showLua", function() Target:showLua() end },
	{ 3, "hideLuaClientside", function() Target:hideLuaClientside() end },
	{ 3, "showLuaClientside", function() Target:showLuaClientside() end },
	{ 3, "luaDisable", function() Switch:luaDisable() end },
	{ 3, "luaEnable", function() Switch:luaEnable() end },
	{ 3, "luaDisable2", function() Switch:luaDisable() end },
}

local function run(i: number)
	local s = steps[i]
	if s == nil then log("DIAG end") return end
	LuauClock.timeout(s[1]):andThen(function()
		s[3]()
		log(`DIAG {t()} step {i} {s[2]} -> isEnabled={Switch.isEnabled}`)
		run(i + 1)
	end)
end

function BeginPlay()
	if (Switch :: ToggleableComponent?) == nil or (Target :: PhysicalComponent?) == nil then log("DIAG slots missing") return end
	Switch.blueprintOnEnabled.Listen(function() log(`DIAG {t()} EVENT OnEnabled isEnabled={Switch.isEnabled}`) end)
	Switch.blueprintOnDisabled.Listen(function() log(`DIAG {t()} EVENT OnDisabled isEnabled={Switch.isEnabled}`) end)
	log(`DIAG {t()} ready isEnabled={Switch.isEnabled}`)
	run(1)
end
