using Microsoft.Win32;
using System.Text.RegularExpressions;

namespace VUltra;

/// <summary>Finds the Vagante install (Steam app 323220) across Steam libraries.</summary>
public static class SteamLocator
{
    const string AppId = "323220";

    public static string? FindVagante()
    {
        foreach (var lib in SteamLibraries())
        {
            var p = Path.Combine(lib, "steamapps", "common", "Vagante", "vagante.exe");
            if (File.Exists(p)) return Path.GetDirectoryName(p);
        }
        return null;
    }

    static IEnumerable<string> SteamLibraries()
    {
        var steam = SteamRoot();
        if (steam == null) yield break;
        var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (!File.Exists(vdf)) { yield return steam; yield break; }

        string text = File.ReadAllText(vdf);
        // "path"   "D:\\SteamLibrary"
        foreach (Match m in Regex.Matches(text, "\"path\"\\s*\"([^\"]+)\"", RegexOptions.IgnoreCase))
            yield return m.Value.Contains('\\') ? Regex.Unescape(m.Groups[1].Value) : m.Groups[1].Value;
        yield return steam;
    }

    static string? SteamRoot()
    {
        try
        {
            using var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            if (k?.GetValue("SteamPath") is string s && Directory.Exists(s)) return s;
        }
        catch { }
        foreach (var d in new[] {
            @"C:\Program Files (x86)\Steam", @"C:\Program Files\Steam" })
            if (Directory.Exists(d)) return d;
        return null;
    }
}
