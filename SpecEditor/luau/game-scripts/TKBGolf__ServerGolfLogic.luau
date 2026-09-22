local function log(any)
    Logging.warn("ServerGolfLogic", any)
    -- print(message)
end
local function debug(any)
    Logging.warn("ServerGolfLogic", any)
    -- print(message)
end

type Score = {
    IsDribble: boolean,
    Score: number
}

type PlayerRow = {
    Player: VRPawn,
    Name: string,
    Score: {Score},
    IsTampered: boolean,
    ID: int
}

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

local ScoreboardsCopy: {    DataTableComponent  } = {}

local function RoundToDecimal(num, numDecimalPlaces)
    local mult = 10^(numDecimalPlaces or 0)
    return math.floor(num * mult + 0.5) / mult
end

local function SetGolfTableStyles(IndividualScoreboard: DataTableComponent) 
    local TRANSPARENT = LinearColor.new(0,0,0,0);
    local HEADERBACKGROUND = LinearColor.new(0.0,0.06,1.0,1);
    local MAINBODYTEXT = LinearColor.new(0,1,1,1);
    local MAINBODYBACKGROUND = LinearColor.new(0.05,0.05,0.05,1);
    local TOTALTEXT = LinearColor.new(1,0,1,1);

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
        }), 2, 2, 2, -2),
        StyleOverRange.new(CellStyle.new({
            backgroundColor = MAINBODYBACKGROUND,
            textColor = TOTALTEXT,
            defaultText = "Total",
        }), 3, -2 , -1, -1),
        StyleOverRange.new(CellStyle.new({
            backgroundColor = TRANSPARENT,
            textColor = TRANSPARENT,
        }), 1, -1, 2, -2)
    });
end

local function RedrawScoreboard(TargetScoreboard: DataTableComponent, Hole: number, DoTransaction: boolean)
    --log("Redrawing Scoreboard")
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


    -- --log(PlayersAtHole[Hole])
    -- --log("Number of players at hole: " .. NumberOfPlayersAtCurrentHole)
    -- --log(DisplayedRows)


    TargetScoreboard:setRowGaps({
        LayoutGap.new(2, 20),
        LayoutGap.new(2 + NumberOfPlayersAtCurrentHole, 10)
    })

    if DoTransaction then         
        TargetScoreboard:beginDataTransaction() 
    end

    local i = 1
    for PlayerID, row in pairs(DisplayedRows) do
        -- --log(`Setting row {row.Name} at {i} {PlayerID}`)

        local CellX = i + 2;
        TargetScoreboard:setCellText(row.Name, CellX, 0)
        
        local NormalTotal = 0
        local DribbleGolfTotal = 0
        local ActiveHole = PlayerAtHole[row.ID] or 0
        -- debug(`Active Hole: {ActiveHole}`)
        -- debug(PlayerAtHole)
        for Score = 1, 9 do
            local CellY = Score + 1;

            if row.Score[Score] == nil or row.Score[Score].Score == 0 then
                TargetScoreboard:setCellText("-", CellX, CellY)
            else
                if row.Score[Score].IsDribble then
                    DribbleGolfTotal = DribbleGolfTotal + row.Score[Score].Score
                    TargetScoreboard:setCellText(`{RoundToDecimal(row.Score[Score].Score, 2)}s`, CellX, CellY)
                else
                    NormalTotal = NormalTotal + row.Score[Score].Score
                    TargetScoreboard:setCellText(`{row.Score[Score].Score}`, CellX, CellY)
                end
            end

            -- if IsEditing then
            --     if SelectedPlayer == PlayerID then
            --         if SelectedHole == Score then
            --             -- --log("Setting selected hole")
            --             TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.10,0.10,0.05,1), CellX, CellY)
            --         else
            --             TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.07,0.07,0.05,1), CellX, CellY)
            --         end
            --     else
            --         TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), CellX, CellY)
            --     end
            -- else 
            TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), CellX, CellY)
            -- end

            if Score == ActiveHole then
                TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.25,0.05,1), CellX, CellY)
            else
                TargetScoreboard:setCellBackgroundColor(LinearColor.new(0.05,0.05,0.05,1), CellX, CellY)
            end
        end

        if row.IsTampered then
            TargetScoreboard:setCellText(`{tostring(NormalTotal)}*`, CellX, 11)
            TargetScoreboard:setCellText(`{RoundToDecimal(DribbleGolfTotal, 2)}s*`, CellX, 12)
        else
            TargetScoreboard:setCellText(tostring(NormalTotal), CellX, 11)
            TargetScoreboard:setCellText(`{RoundToDecimal(DribbleGolfTotal, 2)}s`, CellX, 12)
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

local function InitializeTables (Scoreboards: {DataTableComponent}, Pars: {number}) 
    ScoreboardsCopy = Scoreboards

    for _, IndividualScoreboard in pairs(Scoreboards) do
        IndividualScoreboard:setLayoutStyle(LayoutSettings.new({
            defaultColumnFill = 1,
            columnFill = {4},
            numColumns = 12,
            numRows = 15,
            rowGaps = {
                LayoutGap.new(2, 20)
            },
        }))
    end

    for _, IndividualScoreboard in pairs(Scoreboards) do
        SetGolfTableStyles(IndividualScoreboard)
    end
    
    for ScoreboardID, IndividualScoreboard in pairs(Scoreboards) do
        IndividualScoreboard:beginDataTransaction();
        local NumOfHoles = 9;
        for i = 1, NumOfHoles do
            IndividualScoreboard:setCellText("Hole " .. i, 1, i + 1)
        end
        IndividualScoreboard:setCellText("Par", 2, 1)
        IndividualScoreboard:setCellText("", 1, 1)

        IndividualScoreboard:setCellText("Normal", 1, -2)
        IndividualScoreboard:setCellText("Dribble", 1, -1)
        Pars = {3, 3, 2, 3, 2, 4, 3, 3, 4}
        for i = 1, NumOfHoles do
            IndividualScoreboard:setCellText(tostring(Pars[i]), 2, i + 1)
        end
        RedrawScoreboard(IndividualScoreboard, ScoreboardID, false)

        IndividualScoreboard:endDataTransaction();
    end
end


local function OnPlayerLeave(PlayerID: int)
    ActiveRows[PlayerID] = nil
end

local function EnsurePlayerPartOfRows(PlayerID: int)
    if ActiveRows[PlayerID] then
        return
    end

    --log("Adding player to rows")
    local Player = VRPawn.getPlayerByID(PlayerID)
    if Player then
        Player.onBeforeDestroy:listen(function()
            OnPlayerLeave(PlayerID)
        end)
    else
        -- --log("Player not found")
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

local function EnsurePlayerAtCorrectPosition(PlayerID: int, HoleID: int) 
    -- --log(`[Ensuring] player at correct position: {PlayerID} {HoleID}`)
    if PlayerAtHole[PlayerID] == HoleID then
        return
    end

    local PreviousHole = PlayerAtHole[PlayerID]
    if PreviousHole then
        -- --log(`[Ensuring] Player was at hole {PreviousHole}`)
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
    -- --log(PlayersAtHole)
end

local function RedrawScoreboards()
    
    for ScoreboardID, Scoreboard in pairs(ScoreboardsCopy) do
        RedrawScoreboard(Scoreboard, ScoreboardID, true)
    end
    -- for Hole, Scoreboard_i in pairs(ScoreboardsThatNeedRedrawing) do
    --     RedrawScoreboard(ScoreboardsCopy[Hole], Hole, true)
    -- end
    -- ScoreboardsThatNeedRedrawing = {}
end

return {
    InitializeTables = InitializeTables,
    RoundToDecimal = RoundToDecimal,
    ActiveRows = ActiveRows,
    EnsurePlayerPartOfRows = EnsurePlayerPartOfRows,
    ScoreboardsThatNeedRedrawing = ScoreboardsThatNeedRedrawing,
    RedrawScoreboards = RedrawScoreboards,
    EnsurePlayerAtCorrectPosition = EnsurePlayerAtCorrectPosition
}