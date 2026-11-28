// Surveys the loaded world so the bridge can draw a Takaro map from the server's own data.
//
// The dedicated server ships the terrain (.wrp) but not the map or satellite textures, so the
// map is drawn from what the engine can answer: terrain height, ground surface (Chernarus and
// Livonia paint their roads into it), sea and ponds, building footprints and tree positions. The survey runs once per world in
// small steps on the system call queue (a slice of a terrain row or one object query per step), writes
// $profile:TakaroIntegration/map/<world>.survey and is reused on later starts. The bridge
// renders tiles from that file; nothing here runs per frame once the survey is done.
//
// File format (text, one record per line):
//   takaro-dayz-survey 1
//   world <name> size <metres> cell <metres> cols <n> rows <n> treecell <metres>
//   h <row> <cols x 4 hex digits>    terrain height, (metres + 100) * 10, row 0 = southmost
//   s <row> <cols x 1 char>          ground surface, index into the S table ('.' none, '~' table full)
//   w <row> <cols x 1 char>          '0' land, '1' sea, '2' pond
//   b <x> <z> <fx> <fz> <minx> <minz> <maxx> <maxz>   building: decimetres, forward x1000
//   t <row> <tcols x 2 hex digits>   trees per tree cell, capped at 255
//   S <index char> <surface name>
//   end <buildings> <trees>

class TakaroMapSurvey
{
    static const string MAP_DIR = "$profile:TakaroIntegration/map";
    static const string CODES = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!#$%&()*+,-/:;<=>?@[]^_`{|}";
    static const string HEX = "0123456789abcdef";
    static const int CELL = 10;
    static const int TREE_CELL = 25;
    static const int OBJ_CELL = 200;
    static const int STEP_MS = 10;
    // Terrain samples per step: as many as fit in STEP_BUDGET_MS of game thread, at least
    // STEP_MIN and at most STEP_MAX. GetTickTime only advances per frame, so the budget is
    // measured with the CPU tick counter, calibrated against game time during the first steps.
    static const float STEP_BUDGET_MS = 4.0;
    static const int STEP_MIN = 16;
    static const int STEP_MAX = 768;
    // Frames measured with no survey work before the survey starts, as the A/B baseline.
    static const int IDLE_STEPS = 200;

    private static ref TakaroMapSurvey s_Instance;

    protected FileHandle m_File;
    protected string m_World;
    protected string m_Path;
    protected int m_Size;
    protected int m_Cols;
    protected int m_Row;
    protected int m_Col;
    protected string m_RowH;
    protected string m_RowS;
    protected string m_RowW;
    protected int m_ObjCols;
    protected int m_ObjIndex;
    protected int m_TreeCols;
    protected ref array<int> m_Trees;
    protected ref map<string, int> m_Surfaces;
    protected ref array<string> m_SurfaceNames;
    protected int m_Buildings;
    protected int m_TreeTotal;
    protected int m_StartedAt;
    protected bool m_Running;
    protected int m_TickMark;
    protected float m_TimeMark;
    protected float m_TicksPerMs;
    protected int m_LastTick;
    protected int m_IdleSteps;
    // Frame interval statistics per phase: 0 idle baseline, 1 terrain, 2 objects.
    protected ref array<float> m_FrameSum;
    protected ref array<float> m_FrameMax;
    protected ref array<int> m_FrameCount;

    static TakaroMapSurvey Get()
    {
        if (!s_Instance)
            s_Instance = new TakaroMapSurvey();
        return s_Instance;
    }

    static string WorldName()
    {
        string w;
        GetGame().GetWorldName(w);
        w.ToLower();
        return w;
    }

    static string SurveyPath(string world)
    {
        return MAP_DIR + "/" + world + ".survey";
    }

    // Called once after mission init: survey only when this world has no survey yet.
    static void ScheduleAuto(int delayMs)
    {
        TakaroConfigData cfg = TakaroConfig.Get();
        if (cfg && cfg.MapSurvey == 0)
            return;
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(Get().AutoStart, delayMs, false);
    }

    void AutoStart()
    {
        string world = WorldName();
        if (FileExist(SurveyPath(world)))
        {
            TakaroLog.Info("Map survey for " + world + " exists; reusing it");
            return;
        }
        Start();
    }

    bool IsRunning()
    {
        return m_Running;
    }

    string Status()
    {
        if (!m_Running)
        {
            string world = WorldName();
            if (FileExist(SurveyPath(world)))
                return "Map survey for " + world + " is done (" + SurveyPath(world) + ")";
            return "No map survey for " + world;
        }
        if (m_Row < m_Cols)
            return "Map survey running: terrain row " + m_Row + "/" + m_Cols + ", frame interval idle " + FrameText(0) + ", now " + FrameText(1);
        return "Map survey running: objects " + m_ObjIndex + "/" + (m_ObjCols * m_ObjCols) + ", frame interval idle " + FrameText(0) + ", now " + FrameText(2);
    }

    bool Start()
    {
        if (m_Running)
            return false;
        m_World = WorldName();
        m_Size = GetGame().GetWorld().GetWorldSize();
        if (m_Size <= 0)
        {
            TakaroLog.Warn("Map survey: world size unknown, not surveying");
            return false;
        }
        if (!FileExist(MAP_DIR))
            MakeDirectory(MAP_DIR);
        m_Path = SurveyPath(m_World);
        string partial = m_Path + ".partial";
        if (FileExist(partial))
            DeleteFile(partial);
        m_File = OpenFile(partial, FileMode.WRITE);
        if (!m_File)
        {
            TakaroLog.Error("Map survey: cannot write " + partial);
            return false;
        }
        m_Cols = m_Size / CELL;
        m_ObjCols = (m_Size + OBJ_CELL - 1) / OBJ_CELL;
        m_TreeCols = (m_Size + TREE_CELL - 1) / TREE_CELL;
        m_Trees = new array<int>();
        m_Trees.Resize(m_TreeCols * m_TreeCols);
        for (int i = 0; i < m_Trees.Count(); i++)
            m_Trees[i] = 0;
        m_Surfaces = new map<string, int>();
        m_SurfaceNames = new array<string>();
        m_Row = 0;
        m_Col = 0;
        m_RowH = "";
        m_RowS = "";
        m_RowW = "";
        m_ObjIndex = 0;
        m_Buildings = 0;
        m_TreeTotal = 0;
        m_StartedAt = GetGame().GetTime();
        m_TicksPerMs = 0;
        m_TimeMark = 0;
        m_LastTick = 0;
        m_IdleSteps = 0;
        m_FrameSum = {0.0, 0.0, 0.0};
        m_FrameMax = {0.0, 0.0, 0.0};
        m_FrameCount = {0, 0, 0};
        m_Running = true;

        FPrintln(m_File, "takaro-dayz-survey 1");
        FPrintln(m_File, "world " + m_World + " size " + m_Size + " cell " + CELL + " cols " + m_Cols + " rows " + m_Cols + " treecell " + TREE_CELL);
        TakaroLog.Info("Map survey started: " + m_World + " " + m_Size + " m at " + CELL + " m; measuring " + IDLE_STEPS + " idle frames first");
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.Step, STEP_MS, true);
        return true;
    }

    // Ticks per millisecond of the TickCount counter, measured over at least 0.25 s of game time.
    protected void Calibrate()
    {
        if (m_TicksPerMs > 0)
            return;
        float now = GetGame().GetTickTime();
        if (m_TimeMark <= 0)
        {
            m_TickMark = TickCount(0);
            m_TimeMark = now;
            return;
        }
        if (now - m_TimeMark < 0.25)
            return;
        int ticks = TickCount(m_TickMark);
        if (ticks > 0)
        {
            m_TicksPerMs = ticks / ((now - m_TimeMark) * 1000.0);
            TakaroLog.Info("Map survey: step budget " + STEP_BUDGET_MS + " ms = " + Math.Round(m_TicksPerMs * STEP_BUDGET_MS) + " ticks");
        }
        else
            m_TimeMark = 0;
    }

    protected int Phase()
    {
        if (m_IdleSteps < IDLE_STEPS)
            return 0;
        if (m_Row < m_Cols)
            return 1;
        return 2;
    }

    protected string FrameText(int phase)
    {
        if (!m_FrameCount || m_FrameCount[phase] == 0)
            return "n/a";
        float avg = m_FrameSum[phase] / m_FrameCount[phase];
        return "avg " + (Math.Round(avg * 10) / 10) + " ms, max " + Math.Round(m_FrameMax[phase]) + " ms over " + m_FrameCount[phase] + " frames";
    }

    protected void Step()
    {
        if (!m_Running)
            return;
        // Time since the previous step: one server frame, including the previous step's work.
        int now = TickCount(0);
        if (m_LastTick != 0 && m_TicksPerMs > 0)
        {
            float ms = (now - m_LastTick) / m_TicksPerMs;
            // The counter is a 32-bit int that wraps about every 36 minutes; drop that sample.
            if (ms >= 0 && ms < 60000)
            {
                int phase = Phase();
                m_FrameSum[phase] = m_FrameSum[phase] + ms;
                m_FrameCount[phase] = m_FrameCount[phase] + 1;
                if (ms > m_FrameMax[phase])
                    m_FrameMax[phase] = ms;
            }
        }
        m_LastTick = now;
        Calibrate();
        if (m_IdleSteps < IDLE_STEPS)
        {
            m_IdleSteps++;
            if (m_IdleSteps == IDLE_STEPS)
                TakaroLog.Info("Map survey: idle frame interval " + FrameText(0));
            return;
        }
        if (m_Row < m_Cols)
        {
            SurveyRowChunk();
            return;
        }
        if (m_ObjIndex < m_ObjCols * m_ObjCols)
        {
            SurveyObjects(m_ObjIndex);
            m_ObjIndex++;
            if (m_ObjIndex % 1000 == 0)
                TakaroLog.Info("Map survey: objects " + m_ObjIndex + "/" + (m_ObjCols * m_ObjCols) + ", frame interval " + FrameText(2));
            return;
        }
        Finish();
    }

    protected string RoundStr(float f)
    {
        int v = Math.Round(f);
        return v.ToString();
    }

    protected string Hex4(int v)
    {
        if (v < 0) v = 0;
        if (v > 65535) v = 65535;
        string s = HEX.Get((v >> 12) & 15);
        s += HEX.Get((v >> 8) & 15);
        s += HEX.Get((v >> 4) & 15);
        s += HEX.Get(v & 15);
        return s;
    }

    protected string SurfaceCode(string name)
    {
        if (name == "")
            return ".";
        int idx;
        if (!m_Surfaces.Find(name, idx))
        {
            idx = m_SurfaceNames.Count();
            m_Surfaces.Insert(name, idx);
            m_SurfaceNames.Insert(name);
        }
        if (idx >= CODES.Length())
            return "~";
        return CODES.Get(idx);
    }

    // One slice of the current terrain row; the row is written once it is complete.
    protected void SurveyRowChunk()
    {
        float z = m_Row * CELL + CELL * 0.5;
        int t0 = TickCount(0);
        int budget = m_TicksPerMs * STEP_BUDGET_MS;
        int start = m_Col;
        int col = m_Col;
        // Short per-step strings, appended to the row once: appending every sample to the
        // growing row string copied it each time and cost more than the engine queries.
        string hs = "";
        string ss = "";
        string ws = "";
        while (col < m_Cols && col - start < STEP_MAX)
        {
            if (col - start >= STEP_MIN && (budget <= 0 || TickCount(t0) > budget))
                break;
            float x = col * CELL + CELL * 0.5;
            float h = GetGame().SurfaceY(x, z);
            int hv = Math.Round((h + 100.0) * 10.0);
            hs += Hex4(hv);

            string surface;
            GetGame().SurfaceGetType(x, z, surface);
            ss += SurfaceCode(surface);

            // The sea sits at height 0; only low ground can be sea.
            if (h < 5.0 && GetGame().SurfaceIsSea(x, z))
                ws += "1";
            else if (GetGame().SurfaceIsPond(x, z))
                ws += "2";
            else
                ws += "0";
            col++;
        }
        m_RowH += hs;
        m_RowS += ss;
        m_RowW += ws;
        m_Col = col;
        if (m_Col < m_Cols)
            return;
        FPrintln(m_File, "h " + m_Row + " " + m_RowH);
        FPrintln(m_File, "s " + m_Row + " " + m_RowS);
        FPrintln(m_File, "w " + m_Row + " " + m_RowW);
        m_RowH = "";
        m_RowS = "";
        m_RowW = "";
        m_Col = 0;
        m_Row++;
        if (m_Row % 128 == 0)
            TakaroLog.Info("Map survey: terrain row " + m_Row + "/" + m_Cols + ", frame interval " + FrameText(1));
    }


    protected void SurveyObjects(int index)
    {
        int cx = index % m_ObjCols;
        int cz = index / m_ObjCols;
        float x0 = cx * OBJ_CELL;
        float z0 = cz * OBJ_CELL;
        float mx = x0 + OBJ_CELL * 0.5;
        float mz = z0 + OBJ_CELL * 0.5;
        array<Object> objects = new array<Object>();
        array<CargoBase> proxies = new array<CargoBase>();
        GetGame().GetObjectsAtPosition(Vector(mx, GetGame().SurfaceY(mx, mz), mz), OBJ_CELL * 0.75, objects, proxies);
        for (int i = 0; i < objects.Count(); i++)
        {
            Object obj = objects[i];
            if (!obj)
                continue;
            vector p = obj.GetPosition();
            if (p[0] < x0 || p[0] >= x0 + OBJ_CELL || p[2] < z0 || p[2] >= z0 + OBJ_CELL)
                continue;
            if (obj.IsTree() || obj.IsBush())
            {
                if (!obj.IsTree())
                    continue;
                int tx = p[0] / TREE_CELL;
                int tz = p[2] / TREE_CELL;
                if (tx >= 0 && tz >= 0 && tx < m_TreeCols && tz < m_TreeCols)
                {
                    m_Trees[tz * m_TreeCols + tx] = m_Trees[tz * m_TreeCols + tx] + 1;
                    m_TreeTotal++;
                }
                continue;
            }
            if (!obj.IsBuilding())
                continue;
            vector box[2];
            if (!obj.GetCollisionBox(box))
                obj.ClippingInfo(box);
            float sx = box[1][0] - box[0][0];
            float sz = box[1][2] - box[0][2];
            if (sx < 1.0 || sz < 1.0)
                continue;
            vector dir = obj.GetDirection();
            string line = "b " + RoundStr(p[0] * 10.0) + " " + RoundStr(p[2] * 10.0);
            line += " " + RoundStr(dir[0] * 1000.0) + " " + RoundStr(dir[2] * 1000.0);
            line += " " + RoundStr(box[0][0] * 10.0) + " " + RoundStr(box[0][2] * 10.0);
            line += " " + RoundStr(box[1][0] * 10.0) + " " + RoundStr(box[1][2] * 10.0);
            FPrintln(m_File, line);
            m_Buildings++;
        }
    }

    protected void Finish()
    {
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).Remove(this.Step);
        for (int tz = 0; tz < m_TreeCols; tz++)
        {
            string ts = "";
            for (int tx = 0; tx < m_TreeCols; tx++)
            {
                int n = m_Trees[tz * m_TreeCols + tx];
                if (n > 255) n = 255;
                ts += HEX.Get((n >> 4) & 15);
                ts += HEX.Get(n & 15);
            }
            FPrintln(m_File, "t " + tz + " " + ts);
        }
        for (int i = 0; i < m_SurfaceNames.Count() && i < CODES.Length(); i++)
            FPrintln(m_File, "S " + CODES.Get(i) + " " + m_SurfaceNames[i]);
        FPrintln(m_File, "end " + m_Buildings + " " + m_TreeTotal);
        CloseFile(m_File);
        m_File = null;
        string partial = m_Path + ".partial";
        if (FileExist(m_Path))
            DeleteFile(m_Path);
        if (!CopyFile(partial, m_Path))
            TakaroLog.Error("Map survey: cannot copy " + partial + " to " + m_Path);
        else
            DeleteFile(partial);
        m_Running = false;
        int secs = (GetGame().GetTime() - m_StartedAt) / 1000;
        TakaroLog.Info("Map survey done: " + m_World + ", " + m_Buildings + " buildings, " + m_TreeTotal + " trees, " + m_SurfaceNames.Count() + " surfaces, " + secs + " s -> " + m_Path);
        TakaroLog.Info("Map survey frame interval: idle " + FrameText(0) + "; terrain " + FrameText(1) + "; objects " + FrameText(2));
        m_Trees = null;
    }
}
