--[[
    A2ConsoleUnlock - enables the Unreal command console in A2 / Orion Drift.

    Press the ` / ~ key (UE calls it "Tilde") in game and type commands, e.g.
        open /Game/A2/Maps/Station_Prime/Station_Prime_P
        stat fps
        r.TextureStreaming 0

    Shipping builds have the console code intact but never construct the UConsole
    object that owns the input, so the key does nothing. This creates that object on
    the game viewport and makes sure ` is among UInputSettings::ConsoleKeys.

    Replaces the built-in ConsoleEnablerMod, which on this game only binds F10 and
    unregisters its own ClientRestart hook from inside that hook's callback (so it
    stops re-creating the console after a travel). This mod latches on a flag instead
    of unregistering, guarantees the ` key specifically, and re-creates the console if
    it is ever lost. Run one or the other, not both.

    Keeps working across `open` / level travel: the viewport console is recreated
    whenever it goes away, both on ClientRestart and from a slow polling loop (VR
    startup can bring the viewport up long after mods have started).
]]

local UEHelpers = require("UEHelpers")

local MOD = "[A2ConsoleUnlock]"

-- Console keys to guarantee. "Tilde" is the ` / ~ key; F10 is a fallback for
-- keyboard layouts where ` is awkward or eaten by the OS/overlay.
local WANTED_KEYS = { "Tilde", "F10" }

-- Poll interval for the "did the viewport show up / did the console go away" check.
local POLL_MS = 2000

local ConsoleReady = false

local function log(fmt, ...)
    print(string.format("%s " .. fmt .. "\n", MOD, ...))
end

--- Make sure every key in WANTED_KEYS is present in UInputSettings::ConsoleKeys.
--- Existing bindings are left alone - we only ever append what is missing.
local function EnsureConsoleKeys()
    local InputSettings = StaticFindObject("/Script/Engine.Default__InputSettings")
    if not InputSettings or not InputSettings:IsValid() then
        log("InputSettings not found; console keys unchanged")
        return
    end

    local ConsoleKeys = InputSettings.ConsoleKeys
    local changed = false
    for _, KeyString in ipairs(WANTED_KEYS) do
        -- FindFName, not FName(): a name that does not exist in the pool must not be
        -- created here, and NAME_None as a console key would be meaningless anyway.
        local KeyName = UEHelpers.FindFName(KeyString)
        if KeyName and KeyName ~= NAME_None then
            local present = false
            for i = 1, #ConsoleKeys do
                if ConsoleKeys[i].KeyName == KeyName then
                    present = true
                    break
                end
            end
            if not present then
                ConsoleKeys[#ConsoleKeys + 1].KeyName = KeyName
                log("bound console key: %s", KeyString)
                changed = true
            end
        end
    end

    -- Only report the full list when it actually changed. Otherwise every level
    -- travel would reprint the same three keys.
    if changed then
        local names = {}
        for i = 1, #ConsoleKeys do
            names[#names + 1] = ConsoleKeys[i].KeyName:ToString()
        end
        log("console keys: %s", table.concat(names, ", "))
    end
end

--- Construct the UConsole on the game viewport if it does not have one yet.
--- Must run on the game thread (it constructs a UObject).
local function EnsureConsole()
    local Engine = UEHelpers.GetEngine()
    if not Engine or not Engine:IsValid() then return end

    local GameViewport = Engine.GameViewport
    if not GameViewport or not GameViewport:IsValid() then
        -- Normal during early startup and always true under -nullrhi.
        return
    end

    if GameViewport.ViewportConsole:IsValid() then
        if not ConsoleReady then
            ConsoleReady = true
            EnsureConsoleKeys()   -- console survived the travel; just verify the keys
        end
        return
    end

    -- Prefer the engine's own ConsoleClass; fall back to the base UConsole class.
    local ConsoleClass = Engine.ConsoleClass
    if not ConsoleClass or not ConsoleClass:IsValid() then
        ConsoleClass = StaticFindObject("/Script/Engine.Console")
    end
    if not ConsoleClass or not ConsoleClass:IsValid() then
        log("no Console class available; cannot enable the console")
        return
    end

    local Console = StaticConstructObject(ConsoleClass, GameViewport)
    if not Console or not Console:IsValid() then
        log("StaticConstructObject(UConsole) failed")
        return
    end

    GameViewport.ViewportConsole = Console
    ConsoleReady = true
    log("console created (0x%X) - press ` to open it", Console:GetAddress())
    EnsureConsoleKeys()
end

-- 1. Try immediately, in case the viewport is already up when mods start.
ExecuteInGameThread(EnsureConsole)

-- 2. Re-check when a player controller restarts (map load, `open`, respawn).
--    Deliberately never unregistered - see the header comment.
RegisterHook("/Script/Engine.PlayerController:ClientRestart", function()
    ConsoleReady = false      -- a new viewport may have a fresh (empty) console slot
    EnsureConsole()
end)

-- 3. Slow poll for everything the hook misses: VR startup ordering, viewports that
--    appear late, a console that gets garbage collected on travel. Costs nothing -
--    once a console exists this is a single IsValid() check every POLL_MS.
if type(LoopAsync) == "function" then
    LoopAsync(POLL_MS, function()
        ExecuteInGameThread(function()
            local Engine = UEHelpers.GetEngine()
            if Engine and Engine:IsValid() then
                local vp = Engine.GameViewport
                if vp and vp:IsValid() and not vp.ViewportConsole:IsValid() then
                    ConsoleReady = false
                    EnsureConsole()
                end
            end
        end)
        return false          -- false = keep looping
    end)
end

log("loaded")
