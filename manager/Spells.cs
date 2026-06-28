namespace VUltra;

/// <summary>
/// One toggleable spellbook spell. The Key must match a line in [spellbook_pool] in
/// vultramod.ini (and g_spells[] in dllmain.cpp). The display name and blurb are NOT
/// here - they come from that ini line's inline comment, via IniDocs. Extra = a
/// re-enabled higher-slot spell (needs the 5-bit widening, handled by the DLL).
/// </summary>
public record Spell(string Key, bool Default, bool Extra);

public static class Spells
{
    public static readonly Spell[] All =
    {
        new("dash",            true, false),
        new("fire_shield",     true, false),
        new("teleport",        true, false),
        new("ice_bolt",        true, false),
        new("fireball",        true, false),
        new("lightning",       true, false),
        new("chain_lightning", true, false),
        new("spirits",         true, false),
        new("magic_missile",   true, false),
        new("shockwave",       true, false),
        new("elec_lance",      true, false),
        new("frost_nova",      true, false),
        new("flame_pillar",    true, false),
        new("dark_metamorph",  true, false),
        new("charm",           true, false),
        new("summon_monster",  true, false),
        new("portal",          true, true),
        new("telekinesis",     true, true),
        new("psychic_push",    true, true),
    };

    public const string IniSection = "spellbook_pool";
}
