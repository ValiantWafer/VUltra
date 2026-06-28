using System.Diagnostics;
using System.Reflection;

namespace VUltra;

/// <summary>Handles the proxy-DLL swap, backups, and the ini in the Vagante folder.</summary>
public class ModInstaller(string gameDir)
{
    public string GameDir { get; } = gameDir;
    string Proxy    => Path.Combine(GameDir, "openal32.dll");
    string Real     => Path.Combine(GameDir, "openal32_real.dll");
    string Orig     => Path.Combine(GameDir, "openal32.orig.dll");
    string Ini      => Path.Combine(GameDir, "vultramod.ini");

    public bool IsInstalled => File.Exists(Real) && File.Exists(Orig);
    public static bool IsGameRunning => Process.GetProcessesByName("vagante").Length > 0;

    public void Install(IDictionary<string, bool> pool, bool wispCurse, bool loop, bool forceWisp, int fairyMode, bool fairyStack, int fairyHeal,
                        bool debugWarp, int warpFloor, int loopStartFloor, int startDifficulty,
                        bool mageWandTree, bool mageRandomWand, bool mageWandZero, double harderByPct, int scalingStartsAt,
                        int spawnIncreasePct, int spawnIncreaseStartsAt, int spawnIncreaseMaxPct, int spawnSpreadPct,
                        int spawnSpreadStartsAt, int spawnSpreadMaxPct, bool cursedBomb, bool randomMatchWeapon, bool randomNoTreeDefault, bool randomArcheryBow, bool randomHealth, bool allowMidgameSpectators, bool keepLobbyJoinable)
    {
        if (IsGameRunning) throw new InvalidOperationException("Close Vagante before installing.");
        if (!File.Exists(Proxy)) throw new FileNotFoundException("openal32.dll not found in the game folder.");

        if (!File.Exists(Orig)) File.Copy(Proxy, Orig);          // one-time pristine backup
        File.Copy(Orig, Real, overwrite: true);                  // forward target = real OpenAL
        WriteEmbeddedDll(Proxy);                                  // our proxy takes the slot
        WriteIni(pool, wispCurse, loop, forceWisp, fairyMode, fairyStack, fairyHeal, debugWarp, warpFloor, loopStartFloor, startDifficulty, mageWandTree, mageRandomWand, mageWandZero, harderByPct, scalingStartsAt, spawnIncreasePct, spawnIncreaseStartsAt, spawnIncreaseMaxPct, spawnSpreadPct, spawnSpreadStartsAt, spawnSpreadMaxPct, cursedBomb, randomMatchWeapon, randomNoTreeDefault, randomArcheryBow, randomHealth, allowMidgameSpectators, keepLobbyJoinable);
    }

    public void Uninstall()
    {
        if (IsGameRunning) throw new InvalidOperationException("Close Vagante before uninstalling.");
        if (File.Exists(Orig)) File.Copy(Orig, Proxy, overwrite: true);
        if (File.Exists(Real)) File.Delete(Real);
        // ini + backup kept so a re-install restores prior choices
    }

    static void WriteEmbeddedDll(string dest)
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("VUltra.dll")
                      ?? throw new InvalidOperationException("embedded DLL missing");
        using var f = File.Create(dest);
        s.CopyTo(f);
    }

    public Dictionary<string, bool> ReadPool()
    {
        var d = new Dictionary<string, bool>();
        foreach (var sp in Spells.All) d[sp.Key] = IniDocs.BoolValue(sp.Key, sp.Default);   // defaults from template
        if (!File.Exists(Ini)) return d;
        bool inSection = false;
        foreach (var raw in File.ReadAllLines(Ini))
        {
            var line = raw.Trim();
            if (line.StartsWith('[')) { inSection = line.Equals($"[{Spells.IniSection}]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!inSection || line.Length == 0 || line.StartsWith(';')) continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            var key = line[..eq].Trim();
            var val = StripComment(line[(eq + 1)..]);
            if (d.ContainsKey(key)) d[key] = val is "1" or "true" or "True";
        }
        return d;
    }

    /// <summary>Reads the Wisp Curse background toggle ([backgrounds] wisp_curse). Default off.</summary>
    public bool ReadWispCurse() => ReadBool("backgrounds", "wisp_curse");

    /// <summary>Reads the "force Wisp Curse for everyone" host toggle ([backgrounds] force_wisp_curse). Default off.</summary>
    public bool ReadForceWispCurse() => ReadBool("backgrounds", "force_wisp_curse");

    /// <summary>Reads the New Game Plus / looping toggle ([loop] new_game_plus). Default off.</summary>
    public bool ReadLoop() => ReadBool("loop", "new_game_plus");

    /// <summary>Reads the starting difficulty tier ([loop] start_difficulty): 1=normal, 2=first loop (NG+), 3+ extra scaling. Default 2.</summary>
    public int ReadStartDifficulty() => ReadInt("loop", "start_difficulty", 2);

    /// <summary>Reads the caged-fairy mode ([npc] fairy_mode): 0=Default, 1=None, 2=All levels, 3=One in first biome.</summary>
    public int ReadFairyMode() => ReadInt("npc", "fairy_mode", 0);
    /// <summary>Reads whether campfire fairy blessings stack per-fairy ([npc] fairy_stack_blessing).</summary>
    public bool ReadFairyStack() => ReadInt("npc", "fairy_stack_blessing", 0) != 0;
    /// <summary>Reads HP each fairy adds at the campfire ([npc] fairy_heal). Default 5 (vanilla).</summary>
    public int ReadFairyHeal() => ReadInt("npc", "fairy_heal", 5);

    /// <summary>Reads the debug F11-warp enable ([loop] test_warp). Default off.</summary>
    public bool ReadDebugWarp() => ReadBool("loop", "test_warp");

    /// <summary>Reads the F11 warp target floor ([loop] warp_floor): 0..12. Default 0.</summary>
    public int ReadWarpFloor() => ReadInt("loop", "warp_floor", 0);

    /// <summary>Reads the loop restart floor ([loop] loop_start_floor): 0..12. Default 0.</summary>
    public int ReadLoopStartFloor() => ReadInt("loop", "loop_start_floor", 0);

    /// <summary>Reads the escalating "harder by" percent ([loop] loop_scale_pct): +N% monster HP/damage per tier. Float. Default 50.</summary>
    public double ReadHarderByPct() => ReadDouble("loop", "loop_scale_pct", 50.0);

    /// <summary>Reads the difficulty tier the extra scaling begins at ([loop] scaling_starts_at): 1..100. Default 3.</summary>
    public int ReadScalingStartsAt() => ReadInt("loop", "scaling_starts_at", 3);

    /// <summary>Reads the per-loop enemy-count increase ([loop] spawn_increase_pct): +N% more enemies per loop. Default 0.</summary>
    public int ReadSpawnIncreasePct() => ReadInt("loop", "spawn_increase_pct", 0);

    /// <summary>Reads the loop number the enemy-count increase begins at ([loop] spawn_increase_starts_at): 1..100. Default 3.</summary>
    public int ReadSpawnIncreaseStartsAt() => ReadInt("loop", "spawn_increase_starts_at", 3);

    /// <summary>Reads the cap on the escalated enemy increase ([loop] spawn_increase_max_pct): 0 = no cap. Default 0.</summary>
    public int ReadSpawnIncreaseMaxPct() => ReadInt("loop", "spawn_increase_max_pct", 0);

    /// <summary>Reads the per-loop "spread out" enemy density ([loop] spawn_spread_pct): more distinct spawns. Default 0.</summary>
    public int ReadSpawnSpreadPct() => ReadInt("loop", "spawn_spread_pct", 0);

    /// <summary>Reads the loop number the spread increase begins at ([loop] spawn_spread_starts_at): 1..100. Default 3.</summary>
    public int ReadSpawnSpreadStartsAt() => ReadInt("loop", "spawn_spread_starts_at", 3);

    /// <summary>Reads the cap on the escalated spread ([loop] spawn_spread_max_pct): 0 = no cap. Default 0.</summary>
    public int ReadSpawnSpreadMaxPct() => ReadInt("loop", "spawn_spread_max_pct", 0);

    /// <summary>Reads whether each player spawns with a permanently-cursed bomb ([player] god_cursed_bomb). Default off.</summary>
    public bool ReadCursedBomb() => ReadInt("player", "god_cursed_bomb", 0) != 0;

    /// <summary>Reads the Random-class weapon-match toggle ([player] random_match_weapon). Default off.</summary>
    public bool ReadRandomMatchWeapon() => ReadInt("player", "random_match_weapon", 0) != 0;

    /// <summary>Reads the no-weapon-tree fallback ([player] random_no_tree_default): on=class default weapon, off=fists. Default on.</summary>
    public bool ReadRandomNoTreeDefault() => ReadInt("player", "random_no_tree_default", 1) != 0;

    /// <summary>Reads the Archery-tree bow grant ([player] random_archery_bow): Random char with Archery also starts with a bow + 30 arrows. Default on.</summary>
    public bool ReadRandomArcheryBow() => ReadInt("player", "random_archery_bow", 1) != 0;

    /// <summary>Reads the random-health toggle ([player] random_health): a Random character starts with a randomized max HP (60-100). Default off.</summary>
    public bool ReadRandomHealth() => ReadInt("player", "random_health", 0) != 0;

    /// <summary>Reads the mid-game spectator toggle ([multiplayer] allow_midgame_spectators): host keeps accepting new joiners (as spectators) after the run starts. Default off. Applied at startup.</summary>
    public bool ReadAllowMidgameSpectators() => ReadInt("multiplayer", "allow_midgame_spectators", 0) != 0;

    /// <summary>Reads the keep-lobby-joinable toggle ([multiplayer] keep_lobby_joinable): host keeps advertising the lobby as joinable so non-modded clients still see the JOIN button mid-run. Default off. Applied at startup.</summary>
    public bool ReadKeepLobbyJoinable() => ReadInt("multiplayer", "keep_lobby_joinable", 0) != 0;

    /// <summary>Reads the mage Wand-skill-tree toggle ([mage] wand_skill_tree). Default off.</summary>
    public bool ReadMageWandTree() => ReadBool("mage", "wand_skill_tree");

    /// <summary>Reads the mage random-Wand-weapon toggle ([mage] random_wand_weapon). Default off.</summary>
    public bool ReadMageRandomWand() => ReadBool("mage", "random_wand_weapon");

    /// <summary>Reads the mage wand-zero-charges toggle ([mage] wand_zero_charges). Default off.</summary>
    public bool ReadMageWandZero() => ReadBool("mage", "wand_zero_charges");

    /// <summary>Reads a single integer key from a named ini section. Returns def if missing/invalid.</summary>
    int ReadInt(string section, string key, int def)
    {
        def = IniDocs.IntValue(key, def);   // shipped default comes from the bundled template
        if (!File.Exists(Ini)) return def;
        bool inSection = false;
        foreach (var raw in File.ReadAllLines(Ini))
        {
            var line = raw.Trim();
            if (line.StartsWith('[')) { inSection = line.Equals($"[{section}]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!inSection || line.Length == 0 || line.StartsWith(';')) continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            if (line[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                return int.TryParse(StripComment(line[(eq + 1)..]), out var n) ? n : def;
        }
        return def;
    }

    /// <summary>Reads a single floating-point key from a named ini section. Returns def if missing/invalid.</summary>
    double ReadDouble(string section, string key, double def)
    {
        def = IniDocs.DoubleValue(key, def);   // shipped default comes from the bundled template
        if (!File.Exists(Ini)) return def;
        bool inSection = false;
        foreach (var raw in File.ReadAllLines(Ini))
        {
            var line = raw.Trim();
            if (line.StartsWith('[')) { inSection = line.Equals($"[{section}]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!inSection || line.Length == 0 || line.StartsWith(';')) continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            if (line[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                return double.TryParse(StripComment(line[(eq + 1)..]), System.Globalization.CultureInfo.InvariantCulture, out var d) ? d : def;
        }
        return def;
    }

    /// <summary>Reads a single boolean key from a named ini section. Default off.</summary>
    bool ReadBool(string section, string key)
    {
        bool def = IniDocs.BoolValue(key);   // shipped default comes from the bundled template
        if (!File.Exists(Ini)) return def;
        bool inSection = false;
        foreach (var raw in File.ReadAllLines(Ini))
        {
            var line = raw.Trim();
            if (line.StartsWith('[')) { inSection = line.Equals($"[{section}]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!inSection || line.Length == 0 || line.StartsWith(';')) continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            if (line[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                return StripComment(line[(eq + 1)..]) is "1" or "true" or "True";
        }
        return def;
    }

    /// <summary>Returns the ini value with any inline ";" comment stripped and trimmed.
    /// The template (and thus the written ini) puts trailing comments on spell lines, so a raw
    /// "1 ; Dash: ..." must be reduced to "1" before it is parsed.</summary>
    static string StripComment(string rhs)
    {
        int sc = rhs.IndexOf(';');
        return (sc >= 0 ? rhs[..sc] : rhs).Trim();
    }

    public void WriteIni(IDictionary<string, bool> pool, bool wispCurse, bool loop, bool forceWisp, int fairyMode, bool fairyStack, int fairyHeal,
                         bool debugWarp, int warpFloor, int loopStartFloor, int startDifficulty,
                         bool mageWandTree, bool mageRandomWand, bool mageWandZero, double harderByPct, int scalingStartsAt,
                         int spawnIncreasePct, int spawnIncreaseStartsAt, int spawnIncreaseMaxPct, int spawnSpreadPct,
                         int spawnSpreadStartsAt, int spawnSpreadMaxPct, bool cursedBomb, bool randomMatchWeapon, bool randomNoTreeDefault, bool randomArcheryBow, bool randomHealth, bool allowMidgameSpectators, bool keepLobbyJoinable)
    {
        // Single source of the file's structure and comments is the bundled template
        // (see IniDocs). We only stamp the current values onto it, so the descriptions and
        // titles live in exactly one place and are never regenerated here.
        string inv(double d) => d.ToString(System.Globalization.CultureInfo.InvariantCulture);
        string b(bool v) => v ? "1" : "0";
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["wisp_curse"]               = b(wispCurse),
            ["force_wisp_curse"]         = b(forceWisp),
            ["new_game_plus"]            = b(loop),
            ["start_difficulty"]         = startDifficulty.ToString(),
            ["loop_start_floor"]         = loopStartFloor.ToString(),
            ["scaling_starts_at"]        = scalingStartsAt.ToString(),
            ["loop_scale_pct"]           = inv(harderByPct),
            ["spawn_increase_starts_at"] = spawnIncreaseStartsAt.ToString(),
            ["spawn_increase_pct"]       = spawnIncreasePct.ToString(),
            ["spawn_increase_max_pct"]   = spawnIncreaseMaxPct.ToString(),
            ["spawn_spread_starts_at"]   = spawnSpreadStartsAt.ToString(),
            ["spawn_spread_pct"]         = spawnSpreadPct.ToString(),
            ["spawn_spread_max_pct"]     = spawnSpreadMaxPct.ToString(),
            ["test_warp"]                = b(debugWarp),
            ["warp_floor"]               = warpFloor.ToString(),
            ["fairy_mode"]               = fairyMode.ToString(),
            ["fairy_stack_blessing"]     = b(fairyStack),
            ["fairy_heal"]               = fairyHeal.ToString(),
            ["wand_skill_tree"]          = b(mageWandTree),
            ["random_wand_weapon"]       = b(mageRandomWand),
            ["wand_zero_charges"]        = b(mageWandZero),
            ["god_cursed_bomb"]          = b(cursedBomb),
            ["random_match_weapon"]      = b(randomMatchWeapon),
            ["random_no_tree_default"]   = b(randomNoTreeDefault),
            ["random_archery_bow"]       = b(randomArcheryBow),
            ["random_health"]            = b(randomHealth),
            ["allow_midgame_spectators"] = b(allowMidgameSpectators),
            ["keep_lobby_joinable"]      = b(keepLobbyJoinable),
        };
        foreach (var sp in Spells.All)
            values[sp.Key] = pool.TryGetValue(sp.Key, out var v) && v ? "1" : "0";

        File.WriteAllLines(Ini, ApplyValues(IniDocs.Template, values));
    }

    /// <summary>
    /// Stamps key=value pairs onto the template text, leaving every comment, blank line, and
    /// inline spell comment untouched. Only the value between "=" and any trailing ";" changes.
    /// </summary>
    static IEnumerable<string> ApplyValues(string template, IReadOnlyDictionary<string, string> values)
    {
        foreach (var raw in template.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            var trimmed = line.TrimStart();
            int eq = line.IndexOf('=');
            if (!trimmed.StartsWith(';') && eq > 0)
            {
                var key = line[..eq].Trim();
                if (values.TryGetValue(key, out var val))
                {
                    var rest = line[(eq + 1)..];
                    int sc = rest.IndexOf(';');
                    var comment = sc >= 0 ? "      " + rest[sc..].TrimEnd() : "";
                    yield return $"{line[..(eq + 1)]} {val}{comment}";
                    continue;
                }
            }
            yield return line;
        }
    }
}
