local EventComponent: EventComponent = nil
local Scoreboard: DataTableComponent = nil


local Scoreboard_1: DataTableComponent = nil
local Scoreboard_2: DataTableComponent = nil
local Scoreboard_3: DataTableComponent = nil
local Scoreboard_4: DataTableComponent = nil
local Scoreboard_5: DataTableComponent = nil
local Scoreboard_6: DataTableComponent = nil
local Scoreboard_7: DataTableComponent = nil
local Scoreboard_8: DataTableComponent = nil
local Scoreboard_9: DataTableComponent = nil

local Edit_Up: PhysicalComponent = nil
local Edit_Down: PhysicalComponent = nil
local Edit_Left: PhysicalComponent = nil
local Edit_Right: PhysicalComponent = nil
local Edit_Score_Up: PhysicalComponent = nil
local Edit_Score_Down: PhysicalComponent = nil
local Edit_Toggle: PhysicalComponent = nil
local Edit_Clear: PhysicalComponent = nil
local Edit_Delete: PhysicalComponent = nil

type PlayerRow = {
    Player: VRPawn,
    Name: string,
    Score: {number},
    IsTampered: boolean,
    ID: int
}

-- local ActiveRows: {
--     [int]: PlayerRow
-- } = {
--     [1] = {
--         Name = "Player 1",
--         Score = {1, 2, 3, 4, 0, 0, 0, 0, 0},
--         IsTampered = false
--     },
--     [5] = {
--         Name = "Player 2",
--         Score = {1, 2, 3, 4, 0, 0, 0, 0, 0},
--         IsTampered = false
--     },
--     [3] = {
--         Name = "Player 3",
--         Score = {1, 2, 3, 4, 0, 0, 0, 0, 0},
--         IsTampered = false
--     },
--     [7] = {
--         Name = "Player 4",
--         Score = {1, 2, 3, 4, 0, 0, 0, 0, 0},
--         IsTampered = false
--     }
-- }

local ActiveRows: {
    [int]: PlayerRow
} = {}


local PlayersAtHole: {
    [int]: { 
        [int]: boolean
     }
} = {}

local PlayerAtHole: {
    [int]: int
} = {}

local ScoreboardsThatNeedRedrawing: {
    [int]: boolean
} = {}

local Scoreboards: { DataTableComponent } = {}

function SetDefaultStyles() 
    local TRANSPARENT = LinearColor.new(0,0,0,0);
    local HEADERBACKGROUND = LinearColor.new(0.0,0.06,1.0,1);
	local MAINBODYTEXT = LinearColor.new(0,1,1,1);
	local MAINBODYBACKGROUND = LinearColor.new(0.05,0.05,0.05,1);
	local TOTALTEXT = LinearColor.new(1,0,1,1);



    for _, IndividualScoreboard in pairs(Scoreboards) do
        IndividualScoreboard:setSharedStyles({
            StyleOverRange.new(CellStyle.new({
                backgroundColor = LinearColor.new(0,0,1,0),
                textColor = LinearColor.new(1,1,1,1),
                textJustification = Enum.TextJustify.Right,
            }), 1, 1, 2, 1),
            StyleOverRange.new(CellStyle.new({
                backgroundColor = MAINBODYBACKGROUND,
                textColor = MAINBODYTEXT,
                defaultText = "00",
            }), 3, 2, -1, -1),
            StyleOverRange.new(CellStyle.new({
                defaultText = "Player Name",
                backgroundColor = HEADERBACKGROUND,
            }), 3, 1, -1, 1),
            StyleOverRange.new(CellStyle.new({
                backgroundColor = HEADERBACKGROUND,
            }), 1, 2, 2, -1),
            StyleOverRange.new(CellStyle.new({
                defaultText = "03",
                backgroundColor = HEADERBACKGROUND,
            }), 2, 2, 2, -1),
            StyleOverRange.new(CellStyle.new({
                backgroundColor = MAINBODYBACKGROUND,
                textColor = TOTALTEXT,
                defaultText = "Total",
            }), 3, -1 , -1, -1),
            StyleOverRange.new(CellStyle.new({
                backgroundColor = TRANSPARENT,
                textColor = TRANSPARENT,
            }), 1, -1, 2, -1)
        });
        
    end
end


function InitializeTable()
    warn("Server Course logic Begin Play")
    for _, IndividualScoreboard in pairs(Scoreboards) do
        IndividualScoreboard:setLayoutStyle(LayoutSettings.new({
            defaultColumnFill = 1,
            columnFill = {4},
            numColumns = 11,
            numRows = 15,
            rowGaps = {
                LayoutGap.new(2, 20)
            },
        }))
    end

	
	SetDefaultStyles();

    for ScoreboardID, IndividualScoreboard in pairs(Scoreboards) do
        IndividualScoreboard:beginDataTransaction();
        local NumOfHoles = 9;
        for i = 1, NumOfHoles do
            IndividualScoreboard:setCellText("Hole " .. i, 1, i + 1)
        end
        IndividualScoreboard:setCellText("Par", 2, 1)
        IndividualScoreboard:setCellText("", 1, 1)

        Pars = {3, 3, 2, 3, 2, 4, 3, 3, 4}
        for i = 1, NumOfHoles do
            IndividualScoreboard:setCellText(tostring(Pars[i]), 2, i + 1)
        end
        RedrawScoreboard(ScoreboardID, false)
        IndividualScoreboard:endDataTransaction();
    end
end

local IsEditing: boolean = false
local SelectedPlayer: int = 1
local SelectedHole: int = 4


function RedrawScoreboards()
    for Hole, _ in pairs(ScoreboardsThatNeedRedrawing) do
        RedrawScoreboard(Hole, true)
    end
    ScoreboardsThatNeedRedrawing = {}
    RedrawScoreboard(1, true)
end

function RedrawScoreboard(Hole: int, DoTransaction: boolean)
    --log(Scoreboards)
    --log("Redrawing Scoreboard")
    --log("Selected Player: " .. SelectedPlayer)
    --log("Selected Hole: " .. Hole)
    local TargetScoreboard = Scoreboards[Hole];
    if TargetScoreboard == nil then
        --issue(`Target Scoreboard: {Hole} is nil`)
        return
    end

    PlayersAtHole[Hole] = PlayersAtHole[Hole] or {}


    local DisplayedRows = {}
    local len = 0
    for PlayerAtHole, _ in pairs(PlayersAtHole[Hole]) do
        len += 1
        DisplayedRows[len] = ActiveRows[PlayerAtHole]
    end

    local NumberOfPlayersAtCurrentHole = len

    for PlayerID, row in pairs(ActiveRows) do
        if PlayersAtHole[Hole][PlayerID] == nil then
            len += 1
            DisplayedRows[len] = row
        end
    end


    --log(PlayersAtHole[Hole])
    --log("Number of players at hole: " .. NumberOfPlayersAtCurrentHole)
    --log(DisplayedRows)


    TargetScoreboard:setRowGaps({
        LayoutGap.new(2, 20),
        LayoutGap.new(2 + NumberOfPlayersAtCurrentHole, 10)
    })

    if DoTransaction then         
        TargetScoreboard:beginDataTransaction() 
    end

    local i = 1
    for PlayerID, row in pairs(DisplayedRows) do
        --log(`Setting row {row.Name} at {i} {PlayerID}`)

        local CellX = i + 2;
        TargetScoreboard:setCellText(row.Name, CellX, 0)
        
        local Total = 0
        local ActiveHole = PlayerAtHole[row.ID] or 0
        debug(`Active Hole: {ActiveHole}`)
        debug(PlayerAtHole)
        for Score = 1, 9 do
            local CellY = Score + 1;

            if row.Score[Score] == nil or row.Score[Score] == 0 then
                TargetScoreboard:setCellText("-", CellX, CellY)
            else
                Total = Total + row.Score[Score]
                TargetScoreboard:setCellText(tostring(row.Score[Score]), CellX, CellY)
            end

            if IsEditing then
                if SelectedPlayer == PlayerID then
                    if SelectedHole == Score then
                        -- --log("Setting selected hole")
                        TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.10,0.10,0.05,1), CellX, CellY)
                    else
                        TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.07,0.07,0.05,1), CellX, CellY)
                    end
                else
                    TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), CellX, CellY)
                end
            else 
                TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), CellX, CellY)
            end

            if Score == ActiveHole then
                TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.25,0.05,1), CellX, CellY)
            else
                TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), CellX, CellY)
            end
        end

        if row.IsTampered then
            TargetScoreboard:setCellText(`{tostring(Total)}*`, CellX, 11)
        else
            TargetScoreboard:setCellText(tostring(Total), CellX, 11)
        end
        i = i + 1
    end

    for RemainingRow = i, 14 do
        for j = 1, 10 do
            TargetScoreboard:setCellText("", RemainingRow + 2, j)
            -- TargetScoreboard:clearCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), RemainingRow + 2, j)
            -- TargetScoreboard:setCellBackgroundColor(LinearColor.new(0,0,0,0), RemainingRow + 2, j)
        end
    end
    if DoTransaction then         
        TargetScoreboard:endDataTransaction()
    end
end


function OnPlayerLeave(PlayerID: int)
    ActiveRows[PlayerID] = nil
    RedrawScoreboard(1, true)
end

function EnsurePlayerPartOfRows(PlayerID: int)
    if ActiveRows[PlayerID] then
        return
    end

    --log("Adding player to rows")
    local Player = VRPawn.getPlayerByID(PlayerID)
    if Player then
        Player.onBeforeDestroy:Listen(function()
            OnPlayerLeave(PlayerID)
        end)
    else
        --log("Player not found")
    end

    local NewName = VRPawn.getPlayerName(PlayerID);
    local newRow = {
        Player = Player,
        Name = if NewName == "None" then "Player " .. PlayerID else NewName,
        Score = {},
        IsTampered = false,
        ID = PlayerID
    }
    ActiveRows[PlayerID] = newRow
end

function EnsurePlayerAtCorrectPosition(PlayerID: int, HoleID: int) 
    --log(`[Ensuring] player at correct position: {PlayerID} {HoleID}`)
    if PlayerAtHole[PlayerID] == HoleID then
        return
    end

    local PreviousHole = PlayerAtHole[PlayerID]
    if PreviousHole then
        --log(`[Ensuring] Player was at hole {PreviousHole}`)
        if PlayersAtHole[PreviousHole] == nil then
            PlayersAtHole[PreviousHole] = {}
        end
        PlayersAtHole[PreviousHole][PlayerID] = nil
        ScoreboardsThatNeedRedrawing[PreviousHole] = true
    end
    ScoreboardsThatNeedRedrawing[HoleID] = true

    if PlayersAtHole[HoleID] == nil then
        PlayersAtHole[HoleID] = {}
    end

    PlayersAtHole[HoleID][PlayerID] = true
    PlayerAtHole[PlayerID] = HoleID
    --log(PlayersAtHole)
end


local EventHandlers = {
    ["OnPlayerStartHole"] = function(Data: any)
        EnsurePlayerPartOfRows(Data.PlayerID)
        EnsurePlayerAtCorrectPosition(Data.PlayerID, Data.HoleIndex)
        ActiveRows[Data.PlayerID].Score[Data.HoleIndex] = 0
        RedrawScoreboards()
    end,
    ["BallInCup"] = function(Data: any)
        EnsurePlayerPartOfRows(Data.PlayerID)
        ActiveRows[Data.PlayerID].Score[Data.Cup] = Data.Score

        ScoreboardsThatNeedRedrawing[1] = true
        ScoreboardsThatNeedRedrawing[2] = true
        ScoreboardsThatNeedRedrawing[3] = true
        ScoreboardsThatNeedRedrawing[4] = true
        ScoreboardsThatNeedRedrawing[5] = true
        ScoreboardsThatNeedRedrawing[6] = true
        ScoreboardsThatNeedRedrawing[7] = true
        ScoreboardsThatNeedRedrawing[8] = true
        ScoreboardsThatNeedRedrawing[9] = true
        RedrawScoreboards()
    end
}

local DebugTimer: TimerComponent = nil;


function BeginPlay()
    -- return
    Scoreboard = Scoreboard_1;
    Scoreboards = {
        Scoreboard_1,
        Scoreboard_2,
        Scoreboard_3,
        Scoreboard_4,
        Scoreboard_5,
        Scoreboard_6,
        Scoreboard_7,
        Scoreboard_8,
        Scoreboard_9
    }

    InitializeTable()
    -- DebugTimer.finished.Listen(function()
    --     debugTick()
    -- end)
    -- DebugTimer:start(1)


    EventComponent.serverEventData.Listen(function (Data: any)
        --log(`Handling Event: ${Data.EventType}`)
        if EventHandlers[Data.EventType] then
            --log("Handling Event")
            EventHandlers[Data.EventType](Data)
        else
            --log("Event not handled")
        end
    end)


    -- Edit_Down.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     --log("EDIT: Edit_Down")
    --     -- If not selecting a valid player in the active rows, then set it to the first player
    --     if ActiveRows[SelectedPlayer] == nil then
    --         --log("EDIT: Edit_Down No players selected, Setting selected player to first player")
    --         for PlayerID, _ in pairs(ActiveRows) do
    --             --log("EDIT: Edit_Down No players selected, Setting selected player to " .. PlayerID)
    --             SelectedPlayer = PlayerID
    --             RedrawScoreboard(1, true)
    --             return
    --         end
    --     end

    --     if ActiveRows[SelectedPlayer] == nil then
    --         return
    --     end


    --     SelectNextPlayer = false
    --     for PlayerID, _ in pairs(ActiveRows) do
    --         if SelectNextPlayer then
    --             --log("EDIT: Edit_Down Setting selected player to " .. PlayerID)
    --             SelectedPlayer = PlayerID
    --             RedrawScoreboard(1, true)
    --             return
    --         end

    --         if PlayerID == SelectedPlayer then
    --             SelectNextPlayer = true
    --         end
    --     end
    --     RedrawScoreboard(1, true)
    -- end)

    -- Edit_Up.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     --log("EDIT: Edit_Up")
    --     -- If not selecting a valid player in the active rows, then set it to the first player
    --     if ActiveRows[SelectedPlayer] == nil then
    --         for PlayerID, _ in pairs(ActiveRows) do
    --             --log("EDIT: Edit_Up No players selected, Setting selected player to " .. PlayerID)
    --             SelectedPlayer = PlayerID
    --             RedrawScoreboard(1, true)
    --             return
    --         end
    --     end

    --     if ActiveRows[SelectedPlayer] == nil then
    --         return
    --     end


    --     PreviousPlayer = nil
    --     for PlayerNum, row in pairs(ActiveRows) do
    --         if PlayerNum == SelectedPlayer then
    --             if PreviousPlayer then
    --                 --log("EDIT: Edit_Up Setting selected player to " .. PreviousPlayer)
    --                 SelectedPlayer = PreviousPlayer
    --                 RedrawScoreboard(1, true)
    --                 return
    --             end
    --         end

    --         PreviousPlayer = PlayerNum
    --     end
    --     RedrawScoreboard(1, true)
    -- end)


    -- Edit_Left.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     if SelectedHole == 0 then
    --         SelectedHole = 9
    --     else
    --         SelectedHole = SelectedHole - 1
    --     end
    --     --log("EDIT: Edit_Left Selected Hole: " .. SelectedHole)
    --     RedrawScoreboard(1, true)
    -- end)

    -- Edit_Right.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     if SelectedHole == 9 then
    --         SelectedHole = 0
    --     else
    --         SelectedHole = SelectedHole + 1
    --     end
    --     --log("EDIT: Edit_Right Selected Hole: " .. SelectedHole)
    --     RedrawScoreboard(1, true)
    -- end)


    -- Edit_Score_Up.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     if ActiveRows[SelectedPlayer] == nil then
    --         return
    --     end

    --     ActiveRows[SelectedPlayer].Score[SelectedHole] = (ActiveRows[SelectedPlayer].Score[SelectedHole] or 0) + 1
    --     ActiveRows[SelectedPlayer].IsTampered = true
    --     RedrawScoreboard(1, true)
    -- end)

    -- Edit_Score_Down.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     if ActiveRows[SelectedPlayer] == nil then
    --         return
    --     end

    --     ActiveRows[SelectedPlayer].Score[SelectedHole] = (ActiveRows[SelectedPlayer].Score[SelectedHole] or 0) - 1
    --     ActiveRows[SelectedPlayer].IsTampered = true
    --     RedrawScoreboard(1, true)
    -- end)


    -- Edit_Clear.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     if ActiveRows[SelectedPlayer] == nil then
    --         return
    --     end
    --     ActiveRows[SelectedPlayer].Score = {}
    --     ActiveRows[SelectedPlayer].IsTampered = false
    --     RedrawScoreboard(1, true)
    -- end)


    -- Edit_Delete.onOverlapByPlayerServer.Listen(function(_: number)
    --     if not IsEditing then return end
    --     if ActiveRows[SelectedPlayer] == nil then
    --         return
    --     end
    --     ActiveRows[SelectedPlayer] = nil
    --     RedrawScoreboard(1, true)
    -- end)

    -- Edit_Toggle.onOverlapByPlayerServer.Listen(function(_: number)
    --     -- Add a new testing user
    --     EnsurePlayerPartOfRows(5)
    --     ActiveRows[5].Score = {1, 2, 3, 4, 0, 0, 0, 0, 0}
    --     -- if IsEditing then
    --     --     Edit_Up:hideLua()
    --     --     Edit_Down:hideLua()
    --     --     Edit_Left:hideLua()
    --     --     Edit_Right:hideLua()
    --     --     Edit_Score_Up:hideLua()
    --     --     Edit_Score_Down:hideLua()
    --     --     Edit_Clear:hideLua()
    --     --     Edit_Delete:hideLua()
    --     -- else
    --     --     Edit_Up:showLua()
    --     --     Edit_Down:showLua()
    --     --     Edit_Left:showLua()
    --     --     Edit_Right:showLua()
    --     --     Edit_Score_Up:showLua()
    --     --     Edit_Score_Down:showLua()
    --     --     Edit_Clear:showLua()
    --     --     Edit_Delete:showLua()
    --     -- end
    --     -- IsEditing = not IsEditing
    --     -- RedrawScoreboard(1, true)
    -- end)
    
    -- Edit_Up:hideLua()
    -- Edit_Down:hideLua()
    -- Edit_Left:hideLua()
    -- Edit_Right:hideLua()
    -- Edit_Score_Up:hideLua()
    -- Edit_Score_Down:hideLua()
    -- Edit_Clear:hideLua()
    -- Edit_Delete:hideLua()
end

function Debug_Start(ID, Hole) 
    EventHandlers["OnPlayerStartHole"]({PlayerID = ID, HoleIndex = Hole})
end
function Debug_Score(ID, Hole, Score) 
    EventHandlers["BallInCup"]({PlayerID = ID, Cup = Hole, Score = Score})
end
-- local DebugEvents = {
--     [1] = function ()  Debug_Start(1,1)        end,
--     [2] = function ()  Debug_Start(2,1)        end,
--     [3] = function ()  Debug_Start(3,1)        end,
--     [4] = function ()  Debug_Score(1, 1, 1)    end,
--     [5] = function ()  Debug_Score(2, 1, 2)    end,
--     [6] = function ()  Debug_Start(2,2)        end,
--     [7] = function ()  Debug_Score(2, 2, 2)    end,
--     [8] = function ()  Debug_Score(1, 1, 1)    end,
--     [9] = function ()  Debug_Start(3,3)        end,
--     [10] = function () Debug_Score(3, 3, 3)    end,
--     [11] = function () Debug_Start(4,1)        end,
--     [12] = function () Debug_Score(4, 1, 4)    end,
--     [13] = function () Debug_Score(1, 1, 1)    end,
--     [14] = function () Debug_Start(1,2)        end,
--     [15] = function () Debug_Start(2,3)        end,
--     [16] = function () Debug_Score(2, 3, 2)    end,
--     [17] = function () Debug_Start(5,1)        end,
--     [18] = function () Debug_Start(6,1)        end,
--     [19] = function () Debug_Start(7,1)        end,
--     [20] = function () Debug_Start(8,1)        end,

--     [21] = function () Debug_Score(5, 1, 5)    end,
--     [22] = function () Debug_Score(6, 1, 6)    end,
--     [23] = function () Debug_Score(7, 1, 7)    end,
--     [24] = function () Debug_Score(8, 1, 8)    end,
--     [25] = function () Debug_Start(5,2)        end,
--     [26] = function () Debug_Start(8,2)        end,
--     [27] = function () Debug_Start(7,2)        end,
--     [28] = function () Debug_Start(6,2)        end,

--     [29] = function () Debug_Score(5, 2, 5)    end,
--     [30] = function () Debug_Score(6, 2, 6)    end,
--     [31] = function () Debug_Start(2, 4)        end,
--     [32] = function () Debug_Start(3, 4)        end,
--     [33] = function () Debug_Score(7, 2, 7)    end,
--     [34] = function () Debug_Score(8, 2, 8)    end,
--     [35] = function () Debug_Score(1, 2, 1)    end,
--     [36] = function () Debug_Start(1, 3)        end,
--     [37] = function () Debug_Start(4, 2)        end,
--     [38] = function () Debug_Score(4, 2, 4)    end,

--     [39] = function () Debug_Start(4, 3)        end,
--     [40] = function () Debug_Score(4, 3, 4)    end,
--     [41] = function () Debug_Start(6, 3)        end,
--     [42] = function () Debug_Start(5, 3)        end,
--     [43] = function () Debug_Start(8, 3)        end,
--     [44] = function () Debug_Score(5, 3, 5)    end,
--     [45] = function () Debug_Start(7, 3)        end,
--     [46] = function () Debug_Score(6, 3, 6)    end,
--     [47] = function () Debug_Score(1, 3, 1)    end,
--     [48] = function () Debug_Score(8, 3, 8)    end,
--     [49] = function () Debug_Score(7, 3, 7)    end,
-- }

-- local TickNumber = 0
-- function debugTick() 
--     -- --log(`Tick: {TickNumber}`)
--     TickNumber = TickNumber + 1
--     if DebugEvents[TickNumber] then
--         DebugEvents[TickNumber]()
--     end
--     -- --log("Debug Tick")

--     DebugTimer:start(.05)
-- end