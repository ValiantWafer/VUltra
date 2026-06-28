using System.Reflection;

namespace VUltra;

/// <summary>
/// The single source of every option's title and description: the bundled vultramod.ini.
/// Parsed once at startup. For a normal option, the comment block directly above its
/// "key = value" line is the help text - the first comment line is the Title (the control
/// label) and the rest is the Desc (the tooltip). Spells instead use an inline trailing
/// comment "key = value ; Title: description".
/// </summary>
public static class IniDocs
{
    public static string Template { get; private set; } = "";
    static readonly Dictionary<string, string> _vals = new(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, (string Title, string Desc)> _docs = Load();

    /// <summary>Control label for an option key (falls back to the key itself).</summary>
    public static string Title(string key) => _docs.TryGetValue(key, out var d) && d.Title.Length > 0 ? d.Title : key;

    /// <summary>Tooltip text for an option key (empty if none).</summary>
    public static string Desc(string key) => _docs.TryGetValue(key, out var d) ? d.Desc : "";

    // The template doubles as the single source of DEFAULT values: the value written next to
    // each key in vultramod.ini is what a brand-new user starts with (used by ModInstaller when
    // no game-folder ini exists yet). To change the shipped defaults, edit dll/vultramod.ini.
    public static bool BoolValue(string key, bool fallback = false)
        => _vals.TryGetValue(key, out var v) ? v is "1" or "true" or "True" : fallback;
    public static int IntValue(string key, int fallback = 0)
        => _vals.TryGetValue(key, out var v) && int.TryParse(v, out var n) ? n : fallback;
    public static double DoubleValue(string key, double fallback = 0)
        => _vals.TryGetValue(key, out var v)
           && double.TryParse(v, System.Globalization.CultureInfo.InvariantCulture, out var d) ? d : fallback;

    static Dictionary<string, (string, string)> Load()
    {
        var map = new Dictionary<string, (string, string)>(StringComparer.OrdinalIgnoreCase);
        Template = ReadEmbedded();
        var block = new List<string>();   // comment lines accumulated since the last key/blank/section

        foreach (var raw in Template.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.Trim();
            if (line.StartsWith(';')) { block.Add(line.TrimStart(';').Trim()); continue; }
            if (line.Length == 0 || line.StartsWith('[')) { block.Clear(); continue; }

            int eq = line.IndexOf('=');
            if (eq > 0)
            {
                var key = line[..eq].Trim();
                int sc = line.IndexOf(';', eq);          // inline trailing comment? (spells)
                _vals[key] = (sc >= 0 ? line[(eq + 1)..sc] : line[(eq + 1)..]).Trim();
                if (sc >= 0)
                {
                    var inline = line[(sc + 1)..].Trim();
                    int colon = inline.IndexOf(": ", StringComparison.Ordinal);
                    map[key] = colon > 0
                        ? (inline[..colon].Trim(), inline[(colon + 2)..].Trim())
                        : ("", inline);
                }
                else if (block.Count > 0)
                {
                    map[key] = (block[0], string.Join(" ", block.Skip(1)));
                }
            }
            block.Clear();
        }
        return map;
    }

    static string ReadEmbedded()
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("vultramod.ini");
        if (s == null) return "";
        using var r = new StreamReader(s);
        return r.ReadToEnd();
    }
}
