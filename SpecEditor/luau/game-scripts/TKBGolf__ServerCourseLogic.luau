Utilities = require("ServerGolfLogic")

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

local Scoreboards: { DataTableComponent } = {}


local GolfTees_1: GolfTeeComponent = nil
local GolfTees_2: GolfTeeComponent = nil
local GolfTees_3: GolfTeeComponent = nil
local GolfTees_4: GolfTeeComponent = nil
local GolfTees_5: GolfTeeComponent = nil
local GolfTees_6: GolfTeeComponent = nil
local GolfTees_7: GolfTeeComponent = nil
local GolfTees_8: GolfTeeComponent = nil
local GolfTees_9: GolfTeeComponent = nil
local GolfTeess: { GolfTeeComponent } = {}


local EventHandlers = {
    ["OnPlayerStartHole"] = function(Data: any)
        Utilities.EnsurePlayerPartOfRows(Data.PlayerID)
        Utilities.EnsurePlayerAtCorrectPosition(Data.PlayerID, Data.HoleIndex)
        -- --log(Utilities.ActiveRows)
        -- --log(Utilities.ActiveRows[Data.PlayerID])
        Utilities.ActiveRows[Data.PlayerID].Score[Data.HoleIndex] = {IsDribble = false, Score = 0}
        Utilities.RedrawScoreboards()
    end,
    ["BallInCup"] = function(Data: any)
        Utilities.EnsurePlayerPartOfRows(Data.PlayerID)

        if(Data.Dribble) then
            Utilities.ActiveRows[Data.PlayerID].Score[Data.Cup] = {IsDribble = true, Score = Data.Time}
        else
            Utilities.ActiveRows[Data.PlayerID].Score[Data.Cup] = {IsDribble = false, Score = Data.Score}
    end

        Utilities.ScoreboardsThatNeedRedrawing[1] = true
        Utilities.ScoreboardsThatNeedRedrawing[2] = true
        Utilities.ScoreboardsThatNeedRedrawing[3] = true
        Utilities.ScoreboardsThatNeedRedrawing[4] = true
        Utilities.ScoreboardsThatNeedRedrawing[5] = true
        Utilities.ScoreboardsThatNeedRedrawing[6] = true
        Utilities.ScoreboardsThatNeedRedrawing[7] = true
        Utilities.ScoreboardsThatNeedRedrawing[8] = true
        Utilities.ScoreboardsThatNeedRedrawing[9] = true
        Utilities.RedrawScoreboards()
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

    GolfTeess = {
        GolfTees_1,
        GolfTees_2,
        GolfTees_3,
        GolfTees_4,
        GolfTees_5,
        GolfTees_6,
        GolfTees_7,
        GolfTees_8,
        GolfTees_9
    }
    -- --log(Scoreboard.isInDataTransaction);
    Scoreboard.isInDataTransaction = true;

    Utilities.InitializeTables(Scoreboards,  {3, 3, 2, 3, 2, 4, 3, 3, 4})

    EventComponent.serverEventData.Listen(function (Data: any)
        -- --log(`Handling Event: ${Data.EventType}`)
        if EventHandlers[Data.EventType] then
            -- --log("Handling Event")
            EventHandlers[Data.EventType](Data)
        else
            -- --log("Event not handled")
                end
    end)
    
    Edit_Up:hideLua()
    Edit_Down:hideLua()
    Edit_Left:hideLua()
    Edit_Right:hideLua()
    Edit_Score_Up:hideLua()
    Edit_Score_Down:hideLua()
    Edit_Clear:hideLua()
    Edit_Delete:hideLua()
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