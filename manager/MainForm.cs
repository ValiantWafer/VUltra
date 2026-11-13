
namespace VUltra;

public class MainForm : Form
{
    // palette
    static readonly Color Bg      = Color.FromArgb(0x1E, 0x1E, 0x26);
    static readonly Color Panel   = Color.FromArgb(0x29, 0x29, 0x34);
    static readonly Color Accent  = Color.FromArgb(0x8B, 0x5C, 0xF6);
    static readonly Color Accent2 = Color.FromArgb(0xB9, 0x9A, 0xFF);
    static readonly Color Fg      = Color.FromArgb(0xEC, 0xEC, 0xF1);
    static readonly Color Muted   = Color.FromArgb(0x9A, 0x9A, 0xA8);
    static readonly Color Ok      = Color.FromArgb(0x5C, 0xD6, 0x8A);
    static readonly Color Warn    = Color.FromArgb(0xE0, 0x84, 0x3C);

    ModInstaller? _inst;
    readonly Dictionary<string, CheckBox> _checks = new();
    readonly ToolTip _tip = new() { AutoPopDelay = 12000, InitialDelay = 300 };
    TextBox _pathBox = null!;
    Label _status = null!;
    Label _dirtyLbl = null!;            // "unsaved changes" warning (shown when UI differs from the saved ini)
    Button _btnInstall = null!, _btnSave = null!, _btnUninstall = null!;
    bool _loading;                      // true while SetGame() pushes ini values into the UI (suppress dirty)
    string _savedSig = "";              // signature of the UI the last time it was loaded/saved to the ini
    CheckBox _wispCheck = null!;        // Wisp Curse selectable background (hidden for now; state preserved)
    CheckBox _loopCheck = null!;
    NumericUpDown _startDiffNum = null!;   // tier a fresh run begins at (1=normal, 2=first loop/NG+, ...)
    CheckBox _forceWispCheck = null!;   // force the Wisp Curse floor effect for everyone (host)
    ComboBox _fairyCombo = null!;       // caged-fairy mode: 0=Default, 1=None, 2=All levels
    CheckBox _fairyStackCheck = null!;  // stack campfire fairy blessings (+5 HP per fairy, not per biome)
    NumericUpDown _fairyHealNum = null!;// HP each fairy adds at the campfire (vanilla 5)
    CheckBox _mageWandTreeCheck = null!;   // mage: Wand skill tree instead of Rod
    CheckBox _mageRandomWandCheck = null!; // mage: random Wand weapon instead of Rod
    CheckBox _mageWandZeroCheck = null!;   // mage: starting wand begins at 0 charges
    CheckBox _debugCheck = null!;       // dev tools: enables F11 warp + reveals the target dropdown
    Label _warpLabel = null!;
    ComboBox _warpCombo = null!;        // F11 warp target floor (0..12)
    ComboBox _loopStartCombo = null!;   // where the loop restarts (0..12), separate from F11
    NumericUpDown _harderByNum = null!;     // extra monster HP/damage % per tier (float)
    NumericUpDown _scalingStartsNum = null!;// difficulty tier the extra scaling begins at
    NumericUpDown _spawnPctNum = null!;     // +% more enemies per loop
    NumericUpDown _spawnStartsNum = null!;  // loop number the DUPLICATE increase begins at
    NumericUpDown _spawnMaxNum = null!;     // cap on the escalated duplication (0 = no cap)
    NumericUpDown _spawnSpreadNum = null!;  // +% per loop: spread more distinct enemies across the level
    NumericUpDown _spawnSpreadStartsNum = null!; // loop number the SPREAD increase begins at
    NumericUpDown _spawnSpreadMaxNum = null!;    // cap on the escalated spread (0 = no cap)
    CheckBox _cursedBombCheck = null!;           // each player spawns with a permanently-cursed bomb
    CheckBox _randWeaponCheck = null!;           // Random class: start weapon = first weapon skill tree
    CheckBox _randNoTreeCheck = null!;           // Random class with no weapon tree: default weapon (off = fists)
    CheckBox _randArcheryCheck = null!;          // Random class with Archery tree: also get a bow + 30 arrows
    CheckBox _randHealthCheck = null!;           // Random class: randomized max HP (60-100)
    NumericUpDown _randTreeCountNum = null!;      // Random class: how many skill trees to roll (0-16, stock 4)
    CheckBox _midgameSpecCheck = null!;          // Multiplayer: allow new joiners as spectators mid-game (startup)
    CheckBox _lobbyJoinableCheck = null!;        // Multiplayer: keep lobby advertised joinable mid-game (startup)
    CheckBox _hostSyncCheck = null!;             // Multiplayer: host publishes settings to the lobby / client uses the host's

    static object[] LevelNames() => new object[] {
        "Caves 1", "Caves 2", "Caves 3", "Forest 1", "Forest 2", "Forest 3",
        "Catacombs 1", "Catacombs 2", "Catacombs 3", "Rift 1", "Rift 2", "Rift 3", "Final Boss" };

    public MainForm()
    {
        Text = "V Ultra";
        BackColor = Bg; ForeColor = Fg;
        Font = new Font("Segoe UI", 9f);
        // DPI: every position and size in BuildUi is written in 96-DPI ("100%") units, and
        // ScaleLayout multiplies them by DeviceDpi/96 at the end of the constructor. At that point
        // DeviceDpi is the primary monitor's. If the window then opens on (or is dragged to) a monitor
        // with a different DPI, WinForms rescales from there (AutoScaleMode.Dpi), so it's right
        // everywhere. Fonts are in points, so they already render at the real DPI.
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(660, 840);
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        MinimumSize = new Size(520, 400);
        StartPosition = FormStartPosition.CenterScreen;
        BuildUi();
        Detect();
        ScaleLayout(DeviceDpi / 96f);
    }

    // Multiply the 96-DPI layout by f: the form's size limits, and every control's position, size
    // (auto-sized ones keep their measured size), margins, padding and fixed table rows/columns.
    void ScaleLayout(float f)
    {
        if (Math.Abs(f - 1f) < 0.01f) return;
        int R(float v) => (int)Math.Round(v * f);
        Size Sz(Size z) => new(R(z.Width), R(z.Height));
        Padding Pd(Padding p) => new(R(p.Left), R(p.Top), R(p.Right), R(p.Bottom));
        void Walk(Control parent)
        {
            if (parent is TableLayoutPanel t)
            {
                foreach (RowStyle rs in t.RowStyles) if (rs.SizeType == SizeType.Absolute) rs.Height = R(rs.Height);
                foreach (ColumnStyle cs in t.ColumnStyles) if (cs.SizeType == SizeType.Absolute) cs.Width = R(cs.Width);
            }
            foreach (Control c in parent.Controls)
            {
                if (c.Dock == DockStyle.None)          // docked controls are sized by their parent
                {
                    c.Location = new Point(R(c.Left), R(c.Top));
                    if (!c.AutoSize) c.Size = Sz(c.Size);
                    else if (c is TextBoxBase) c.Width = R(c.Width);   // auto-sizes its height only
                }
                c.Margin = Pd(c.Margin);
                c.Padding = Pd(c.Padding);
                if (c.MaximumSize != Size.Empty) c.MaximumSize = Sz(c.MaximumSize);
                Walk(c);
            }
        }
        var client = Sz(ClientSize);
        SuspendLayout();
        Walk(this);
        ResumeLayout(true);
        MinimumSize = Sz(MinimumSize);
        ClientSize = client;               // set last, after the children are scaled
    }

    // After DPI scaling the window can be taller (or wider) than the screen, e.g. 840 rows at 150%
    // is 1260 pixels. Shrink it to the monitor's work area and re-center; the sections scroll.
    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        var area = Screen.FromControl(this).WorkingArea;
        int w = Math.Min(Width, area.Width), h = Math.Min(Height, area.Height);
        if (w != Width || h != Height)
        {
            Size = new Size(w, h);
            Location = new Point(area.Left + (area.Width - w) / 2, area.Top + (area.Height - h) / 2);
        }
    }

    // Every option section lives in a CollapsibleSection so it can be folded away. They are stacked
    // in a single top-down FlowLayoutPanel, which re-flows the ones below whenever one is toggled.
    void BuildUi()
    {
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, BackColor = Bg };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 68));   // path
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));   // sections
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 140));  // footer

        // ---- game path row ----
        var pathPanel = new Panel { Dock = DockStyle.Fill };
        var lblGame = new Label { Text = "Game folder:", AutoSize = true, ForeColor = Muted, Location = new Point(18, 6) };
        _pathBox = new TextBox { ReadOnly = true, BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
            Location = new Point(18, 27), Width = 482 };
        var browse = MakeButton("Browse…", 510, 25, 120, secondary: true);
        browse.Click += (_, _) => Browse();
        pathPanel.Controls.AddRange(new Control[] { lblGame, _pathBox, browse });
        root.Controls.Add(pathPanel, 0, 0);

        // ---- collapsible sections host ----
        var flow = new FlowLayoutPanel {
            Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false,
            AutoScroll = true, BackColor = Bg, Padding = new Padding(4, 4, 4, 4) };
        const int secW = 620;
        CollapsibleSection Section(string title)
        {
            var s = new CollapsibleSection(title, Accent2, secW);
            flow.Controls.Add(s);
            return s;
        }

        int x0 = 22, subX = x0 + 20;
        // Place an input control just past the rendered right edge of its label, so longer labels
        // never get shoved into. PreferredWidth is measured at the real DPI, but positions here are
        // 96-DPI units that ScaleLayout scales afterwards, so convert the measured width to 96-DPI units.
        int After(Label l, int gap = 14) => l.Location.X + l.PreferredWidth * 96 / DeviceDpi + gap;

        // ===== STANDARD SPELLBOOK SPELLS =====
        {
            var body = Section("STANDARD SPELLBOOK SPELLS").Body;
            var normals = Spells.All.Where(s => !s.Extra).ToArray();
            int y0 = 6, colW = 300, rh = 28, rows = (normals.Length + 1) / 2;
            for (int i = 0; i < normals.Length; i++)
                body.Controls.Add(MakeCheck(normals[i], x0 + (i / rows) * colW, y0 + (i % rows) * rh));
        }

        // ===== EXTRA SPELLS =====
        {
            var body = Section("EXTRA SPELLS (REQUIRES HOST AND CLIENT TO HAVE MOD)").Body;
            var extras = Spells.All.Where(s => s.Extra).ToArray();
            int y0 = 6, colW = 300, rh = 28;
            for (int i = 0; i < extras.Length; i++)
                body.Controls.Add(MakeCheck(extras[i], x0 + (i % 2) * colW, y0 + (i / 2) * rh));
        }

        // Wisp Curse selectable-background toggle is hidden for now. The checkbox is kept off-form so
        // its ini state ([backgrounds] wisp_curse) is preserved on Save/Install without being shown.
        _wispCheck = new CheckBox { Checked = false };

        // The sections below hold full-height dropdowns and spin boxes, not just short checkboxes,
        // so give their rows more vertical room than the compact spell grid used above.
        const int rowH = 36;

        // ===== LOOPING ===== (its own section: New Game Plus + all the per-loop scaling knobs)
        {
            var body = Section("LOOPING").Body;
            _loopCheck = new CheckBox {
                Text = IniDocs.Title("new_game_plus"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6), FlatStyle = FlatStyle.Flat };
            Tip(_loopCheck, IniDocs.Desc("new_game_plus"));
            body.Controls.Add(_loopCheck);

            // (1) Start New Game Plus (sub-options): where a run begins and where loops restart.
            int startDiffY = 6 + rowH + 6;
            var startDiffLbl = new Label { Text = IniDocs.Title("start_difficulty"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, startDiffY + 4) };
            string startDiffTip = IniDocs.Desc("start_difficulty");
            Tip(startDiffLbl, startDiffTip);
            body.Controls.Add(startDiffLbl);
            _startDiffNum = new NumericUpDown {
                Minimum = 1, Maximum = 100, Value = 2, Width = 56,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(startDiffLbl), startDiffY) };
            Tip(_startDiffNum, startDiffTip);
            body.Controls.Add(_startDiffNum);

            int loopStartY = startDiffY + rowH;
            var loopStartLbl = new Label { Text = IniDocs.Title("loop_start_floor"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, loopStartY + 4) };
            string loopStartTip = IniDocs.Desc("loop_start_floor");
            Tip(loopStartLbl, loopStartTip);
            body.Controls.Add(loopStartLbl);
            _loopStartCombo = new ComboBox {
                DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
                BackColor = Panel, ForeColor = Fg, Width = 160, Location = new Point(After(loopStartLbl), loopStartY) };
            _loopStartCombo.Items.AddRange(LevelNames());
            _loopStartCombo.SelectedIndex = 0;
            Tip(_loopStartCombo, loopStartTip);
            body.Controls.Add(_loopStartCombo);

            // (2) SPREAD block: more DISTINCT spawns across the level.
            int spreadStartY = loopStartY + rowH + 6;
            string spreadStartTip = IniDocs.Desc("spawn_spread_starts_at");
            var spreadStartLbl = new Label { Text = IniDocs.Title("spawn_spread_starts_at"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, spreadStartY + 4) };
            Tip(spreadStartLbl, spreadStartTip);
            body.Controls.Add(spreadStartLbl);
            _spawnSpreadStartsNum = new NumericUpDown {
                Minimum = 1, Maximum = 100, Value = 3, Width = 56,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(spreadStartLbl), spreadStartY) };
            Tip(_spawnSpreadStartsNum, spreadStartTip);
            body.Controls.Add(_spawnSpreadStartsNum);

            int spawnSpreadY = spreadStartY + rowH;
            string spreadTip = IniDocs.Desc("spawn_spread_pct");
            var spawnSpreadLbl = new Label { Text = IniDocs.Title("spawn_spread_pct"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, spawnSpreadY + 4) };
            Tip(spawnSpreadLbl, spreadTip);
            body.Controls.Add(spawnSpreadLbl);
            _spawnSpreadNum = new NumericUpDown {
                Minimum = 0, Maximum = 10000, Value = 0, Increment = 50, Width = 84,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(spawnSpreadLbl), spawnSpreadY) };
            Tip(_spawnSpreadNum, spreadTip);
            body.Controls.Add(_spawnSpreadNum);
            var spawnSpreadSuffix = new Label { Text = "% per loop", AutoSize = true, ForeColor = Fg,
                Location = new Point(_spawnSpreadNum.Right + 6, spawnSpreadY + 4) };
            Tip(spawnSpreadSuffix, spreadTip);
            body.Controls.Add(spawnSpreadSuffix);

            int spreadMaxY = spawnSpreadY + rowH;
            string spreadMaxTip = IniDocs.Desc("spawn_spread_max_pct");
            var spreadMaxLbl = new Label { Text = IniDocs.Title("spawn_spread_max_pct"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, spreadMaxY + 4) };
            Tip(spreadMaxLbl, spreadMaxTip);
            body.Controls.Add(spreadMaxLbl);
            _spawnSpreadMaxNum = new NumericUpDown {
                Minimum = 0, Maximum = 100000, Value = 0, Increment = 100, Width = 84,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(spreadMaxLbl), spreadMaxY) };
            Tip(_spawnSpreadMaxNum, spreadMaxTip);
            body.Controls.Add(_spawnSpreadMaxNum);
            var spreadMaxSuffix = new Label { Text = "% (0 = no cap)", AutoSize = true, ForeColor = Fg,
                Location = new Point(_spawnSpreadMaxNum.Right + 6, spreadMaxY + 4) };
            Tip(spreadMaxSuffix, spreadMaxTip);
            body.Controls.Add(spreadMaxSuffix);

            // (3) DUPLICATE block: stack copies on each enemy.
            int spawnStartY = spreadMaxY + rowH + 6;
            string dupStartTip = IniDocs.Desc("spawn_increase_starts_at");
            var spawnStartLbl = new Label { Text = IniDocs.Title("spawn_increase_starts_at"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, spawnStartY + 4) };
            Tip(spawnStartLbl, dupStartTip);
            body.Controls.Add(spawnStartLbl);
            _spawnStartsNum = new NumericUpDown {
                Minimum = 1, Maximum = 100, Value = 3, Width = 56,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(spawnStartLbl), spawnStartY) };
            Tip(_spawnStartsNum, dupStartTip);
            body.Controls.Add(_spawnStartsNum);

            int spawnPctY = spawnStartY + rowH;
            string dupTip = IniDocs.Desc("spawn_increase_pct");
            var spawnPctLbl = new Label { Text = IniDocs.Title("spawn_increase_pct"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, spawnPctY + 4) };
            Tip(spawnPctLbl, dupTip);
            body.Controls.Add(spawnPctLbl);
            _spawnPctNum = new NumericUpDown {
                Minimum = 0, Maximum = 100000000, Value = 0, Increment = 25, Width = 104,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(spawnPctLbl), spawnPctY) };
            Tip(_spawnPctNum, dupTip);
            body.Controls.Add(_spawnPctNum);
            var spawnPctSuffix = new Label { Text = "% per loop", AutoSize = true, ForeColor = Fg,
                Location = new Point(_spawnPctNum.Right + 6, spawnPctY + 4) };
            Tip(spawnPctSuffix, dupTip);
            body.Controls.Add(spawnPctSuffix);

            int spawnMaxY = spawnPctY + rowH;
            string spawnMaxTip = IniDocs.Desc("spawn_increase_max_pct");
            var spawnMaxLbl = new Label { Text = IniDocs.Title("spawn_increase_max_pct"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, spawnMaxY + 4) };
            Tip(spawnMaxLbl, spawnMaxTip);
            body.Controls.Add(spawnMaxLbl);
            _spawnMaxNum = new NumericUpDown {
                Minimum = 0, Maximum = 100000, Value = 0, Increment = 100, Width = 84,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(spawnMaxLbl), spawnMaxY) };
            Tip(_spawnMaxNum, spawnMaxTip);
            body.Controls.Add(_spawnMaxNum);
            var spawnMaxSuffix = new Label { Text = "% (0 = no cap)", AutoSize = true, ForeColor = Fg,
                Location = new Point(_spawnMaxNum.Right + 6, spawnMaxY + 4) };
            Tip(spawnMaxSuffix, spawnMaxTip);
            body.Controls.Add(spawnMaxSuffix);

            // (4) Extra enemy scaling block: beyond-NG+ monster HP/damage.
            int scaleStartY = spawnMaxY + rowH + 6;
            var scaleStartLbl = new Label { Text = IniDocs.Title("scaling_starts_at"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, scaleStartY + 4) };
            string scaleStartTip = IniDocs.Desc("scaling_starts_at");
            Tip(scaleStartLbl, scaleStartTip);
            body.Controls.Add(scaleStartLbl);
            _scalingStartsNum = new NumericUpDown {
                Minimum = 1, Maximum = 100, Value = 3, Width = 56,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(scaleStartLbl), scaleStartY) };
            Tip(_scalingStartsNum, scaleStartTip);
            body.Controls.Add(_scalingStartsNum);

            int harderY = scaleStartY + rowH;
            string harderTip = IniDocs.Desc("loop_scale_pct");
            var harderLbl = new Label { Text = IniDocs.Title("loop_scale_pct"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, harderY + 4) };
            Tip(harderLbl, harderTip);
            body.Controls.Add(harderLbl);
            _harderByNum = new NumericUpDown {
                Minimum = 0, Maximum = 1000, Value = 50, DecimalPlaces = 1, Increment = 5, Width = 72,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(harderLbl), harderY) };
            Tip(_harderByNum, harderTip);
            body.Controls.Add(_harderByNum);
            var harderPctLbl = new Label { Text = "% per tier", AutoSize = true, ForeColor = Fg,
                Location = new Point(_harderByNum.Right + 6, harderY + 4) };
            Tip(harderPctLbl, harderTip);
            body.Controls.Add(harderPctLbl);
        }

        // ===== GAME MODE ===== (everything that is not looping)
        {
            var body = Section("GAME MODE").Body;
            _forceWispCheck = new CheckBox {
                Text = IniDocs.Title("force_wisp_curse"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6), FlatStyle = FlatStyle.Flat };
            Tip(_forceWispCheck, IniDocs.Desc("force_wisp_curse"));
            body.Controls.Add(_forceWispCheck);

            int fairyY = 6 + rowH + 4;
            var fairyLbl = new Label { Text = IniDocs.Title("fairy_mode"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, fairyY + 4) };
            string fairyTip = IniDocs.Desc("fairy_mode");
            Tip(fairyLbl, fairyTip);
            body.Controls.Add(fairyLbl);
            _fairyCombo = new ComboBox {
                DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
                BackColor = Panel, ForeColor = Fg, Width = 240, Location = new Point(After(fairyLbl), fairyY) };
            _fairyCombo.Items.AddRange(new object[] { "Default", "None", "All Levels", "One In First Biome (Classic)" });
            _fairyCombo.SelectedIndex = 0;
            Tip(_fairyCombo, fairyTip);
            body.Controls.Add(_fairyCombo);

            int fairyStackY = fairyY + rowH;
            _fairyStackCheck = new CheckBox {
                Text = IniDocs.Title("fairy_stack_blessing"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, fairyStackY), FlatStyle = FlatStyle.Flat };
            Tip(_fairyStackCheck, IniDocs.Desc("fairy_stack_blessing"));
            body.Controls.Add(_fairyStackCheck);

            int fairyHealY = fairyStackY + rowH;
            string fairyHealTip = IniDocs.Desc("fairy_heal");
            var fairyHealLbl = new Label { Text = IniDocs.Title("fairy_heal"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, fairyHealY + 4) };
            Tip(fairyHealLbl, fairyHealTip);
            body.Controls.Add(fairyHealLbl);
            _fairyHealNum = new NumericUpDown {
                Minimum = 0, Maximum = 1000, Value = 5, Width = 64,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(fairyHealLbl), fairyHealY) };
            Tip(_fairyHealNum, fairyHealTip);
            body.Controls.Add(_fairyHealNum);

            int cursedBombY = fairyHealY + rowH;
            _cursedBombCheck = new CheckBox {
                Text = IniDocs.Title("god_cursed_bomb"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, cursedBombY), FlatStyle = FlatStyle.Flat };
            Tip(_cursedBombCheck, IniDocs.Desc("god_cursed_bomb"));
            body.Controls.Add(_cursedBombCheck);

            int randWeaponY = cursedBombY + rowH;
            _randWeaponCheck = new CheckBox {
                Text = IniDocs.Title("random_match_weapon"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, randWeaponY), FlatStyle = FlatStyle.Flat };
            Tip(_randWeaponCheck, IniDocs.Desc("random_match_weapon"));
            body.Controls.Add(_randWeaponCheck);

            int noTreeY = randWeaponY + rowH;
            _randNoTreeCheck = new CheckBox {
                Text = IniDocs.Title("random_no_tree_default"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, noTreeY), FlatStyle = FlatStyle.Flat };
            Tip(_randNoTreeCheck, IniDocs.Desc("random_no_tree_default"));
            body.Controls.Add(_randNoTreeCheck);

            int archeryY = noTreeY + rowH;
            _randArcheryCheck = new CheckBox {
                Text = IniDocs.Title("random_archery_bow"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, archeryY), FlatStyle = FlatStyle.Flat };
            Tip(_randArcheryCheck, IniDocs.Desc("random_archery_bow"));
            body.Controls.Add(_randArcheryCheck);

            int wandZeroY = archeryY + rowH;
            _mageWandZeroCheck = new CheckBox {
                Text = IniDocs.Title("wand_zero_charges"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, wandZeroY), FlatStyle = FlatStyle.Flat };
            Tip(_mageWandZeroCheck, IniDocs.Desc("wand_zero_charges"));
            body.Controls.Add(_mageWandZeroCheck);

            int randHealthY = wandZeroY + rowH;
            _randHealthCheck = new CheckBox {
                Text = IniDocs.Title("random_health"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, randHealthY), FlatStyle = FlatStyle.Flat };
            Tip(_randHealthCheck, IniDocs.Desc("random_health"));
            body.Controls.Add(_randHealthCheck);

            int randTreeY = randHealthY + rowH;
            string randTreeTip = IniDocs.Desc("random_tree_count");
            var randTreeLbl = new Label { Text = IniDocs.Title("random_tree_count"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, randTreeY + 4) };
            Tip(randTreeLbl, randTreeTip);
            body.Controls.Add(randTreeLbl);
            _randTreeCountNum = new NumericUpDown {
                Minimum = 0, Maximum = 16, Value = 4, Width = 64,
                BackColor = Panel, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(After(randTreeLbl), randTreeY) };
            Tip(_randTreeCountNum, randTreeTip);
            body.Controls.Add(_randTreeCountNum);
        }

        // ===== MULTIPLAYER ===== (HOST ONLY, except host_settings_sync which both need). The portal fixes
        // (portal_coop, portal_two_way) are always on and deliberately have no checkboxes.
        {
            var body = Section("MULTIPLAYER").Body;
            _midgameSpecCheck = new CheckBox {
                Text = IniDocs.Title("allow_midgame_spectators"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6), FlatStyle = FlatStyle.Flat };
            Tip(_midgameSpecCheck, IniDocs.Desc("allow_midgame_spectators"));
            body.Controls.Add(_midgameSpecCheck);

            _lobbyJoinableCheck = new CheckBox {
                Text = IniDocs.Title("keep_lobby_joinable"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6 + rowH), FlatStyle = FlatStyle.Flat };
            Tip(_lobbyJoinableCheck, IniDocs.Desc("keep_lobby_joinable"));
            body.Controls.Add(_lobbyJoinableCheck);

            _hostSyncCheck = new CheckBox {
                Text = IniDocs.Title("host_settings_sync"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6 + rowH * 2), FlatStyle = FlatStyle.Flat };
            Tip(_hostSyncCheck, IniDocs.Desc("host_settings_sync"));
            body.Controls.Add(_hostSyncCheck);
        }

        // ===== MAGE =====
        {
            var body = Section("MAGE  (HOST OVERRIDES ALL MAGES)").Body;
            _mageWandTreeCheck = new CheckBox {
                Text = IniDocs.Title("wand_skill_tree"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6), FlatStyle = FlatStyle.Flat };
            Tip(_mageWandTreeCheck, IniDocs.Desc("wand_skill_tree"));
            body.Controls.Add(_mageWandTreeCheck);
            _mageRandomWandCheck = new CheckBox {
                Text = IniDocs.Title("random_wand_weapon"), AutoSize = true, ForeColor = Fg,
                Location = new Point(x0, 6 + rowH), FlatStyle = FlatStyle.Flat };
            Tip(_mageRandomWandCheck, IniDocs.Desc("random_wand_weapon"));
            body.Controls.Add(_mageRandomWandCheck);
        }

        // ===== DEBUG ===== (F11 warp enable + target dropdown, dropdown hidden until Debug is on)
        {
            var body = Section("DEBUG").Body;
            _debugCheck = new CheckBox {
                Text = IniDocs.Title("test_warp"), AutoSize = true, ForeColor = Warn,
                Location = new Point(x0, 6), FlatStyle = FlatStyle.Flat };
            Tip(_debugCheck, IniDocs.Desc("test_warp"));
            body.Controls.Add(_debugCheck);

            int warpY = 6 + rowH + 4;
            _warpLabel = new Label { Text = IniDocs.Title("warp_floor"), AutoSize = true, ForeColor = Fg,
                Location = new Point(subX, warpY + 4), Visible = false };
            string warpTip = IniDocs.Desc("warp_floor");
            Tip(_warpLabel, warpTip);
            body.Controls.Add(_warpLabel);
            _warpCombo = new ComboBox {
                DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
                BackColor = Panel, ForeColor = Fg, Width = 160, Location = new Point(After(_warpLabel), warpY), Visible = false };
            _warpCombo.Items.AddRange(LevelNames());
            _warpCombo.SelectedIndex = 0;
            Tip(_warpCombo, warpTip);
            body.Controls.Add(_warpCombo);

            _debugCheck.CheckedChanged += (_, _) => { _warpLabel.Visible = _debugCheck.Checked; _warpCombo.Visible = _debugCheck.Checked; };
        }

        root.Controls.Add(flow, 0, 1);

        // ---- footer ----
        var footer = new Panel { Dock = DockStyle.Fill };
        _btnSave      = MakeButton("Update Settings", 18, 12, 170);
        _btnInstall   = MakeButton("Install", 198, 12, 150, secondary: true);
        _btnUninstall = MakeButton("Uninstall", 358, 12, 130, secondary: true);
        var openFolder = MakeButton("Open folder", 498, 12, 130, secondary: true);
        var btnReset  = MakeButton("Reset to Defaults", 18, 52, 190, secondary: true);
        _dirtyLbl = new Label { AutoSize = true, ForeColor = Warn, Visible = false,
            Font = new Font("Segoe UI Semibold", 9f), Location = new Point(222, 60) };
        _status = new Label { AutoSize = true, ForeColor = Muted, Location = new Point(20, 96), MaximumSize = new Size(620, 0) };
        _btnInstall.Click   += (_, _) => DoInstall();
        _btnSave.Click      += (_, _) => DoSave();
        _btnUninstall.Click += (_, _) => DoUninstall();
        btnReset.Click      += (_, _) => ResetToDefaults();
        openFolder.Click    += (_, _) => { if (_inst != null) try { System.Diagnostics.Process.Start("explorer.exe", _inst.GameDir); } catch { } };
        footer.Controls.AddRange(new Control[] { _btnInstall, _btnSave, _btnUninstall, openFolder, btnReset, _dirtyLbl, _status });
        root.Controls.Add(footer, 0, 2);

        Controls.Add(root);
        WireDirtyTracking(this);   // any input change flips the "unsaved changes" warning on
    }

    // Reset every control to the shipped default (the value written next to its key in the bundled
    // vultramod.ini, via IniDocs). Leaves the change "unsaved" so the user clicks Update/Install to apply.
    void ResetToDefaults()
    {
        _loading = true;   // suppress per-change dirty churn while we set everything
        foreach (var (k, cb) in _checks) cb.Checked = IniDocs.BoolValue(k, ((Spell)cb.Tag!).Default);
        _wispCheck.Checked = false;
        _loopCheck.Checked = IniDocs.BoolValue("new_game_plus");
        _startDiffNum.Value = ClampNum(IniDocs.IntValue("start_difficulty", 2), _startDiffNum);
        _loopStartCombo.SelectedIndex = Math.Clamp(IniDocs.IntValue("loop_start_floor", 0), 0, _loopStartCombo.Items.Count - 1);
        _spawnSpreadStartsNum.Value = ClampNum(IniDocs.IntValue("spawn_spread_starts_at", 3), _spawnSpreadStartsNum);
        _spawnSpreadNum.Value = ClampNum(IniDocs.IntValue("spawn_spread_pct", 0), _spawnSpreadNum);
        _spawnSpreadMaxNum.Value = ClampNum(IniDocs.IntValue("spawn_spread_max_pct", 0), _spawnSpreadMaxNum);
        _spawnStartsNum.Value = ClampNum(IniDocs.IntValue("spawn_increase_starts_at", 3), _spawnStartsNum);
        _spawnPctNum.Value = ClampNum(IniDocs.IntValue("spawn_increase_pct", 0), _spawnPctNum);
        _spawnMaxNum.Value = ClampNum(IniDocs.IntValue("spawn_increase_max_pct", 0), _spawnMaxNum);
        _scalingStartsNum.Value = ClampNum(IniDocs.IntValue("scaling_starts_at", 3), _scalingStartsNum);
        _harderByNum.Value = (decimal)Math.Clamp(IniDocs.DoubleValue("loop_scale_pct", 50), (double)_harderByNum.Minimum, (double)_harderByNum.Maximum);
        _forceWispCheck.Checked = IniDocs.BoolValue("force_wisp_curse");
        _fairyCombo.SelectedIndex = Math.Clamp(IniDocs.IntValue("fairy_mode", 0), 0, _fairyCombo.Items.Count - 1);
        _fairyStackCheck.Checked = IniDocs.BoolValue("fairy_stack_blessing");
        _fairyHealNum.Value = ClampNum(IniDocs.IntValue("fairy_heal", 5), _fairyHealNum);
        _cursedBombCheck.Checked = IniDocs.BoolValue("god_cursed_bomb");
        _randWeaponCheck.Checked = IniDocs.BoolValue("random_match_weapon");
        _randNoTreeCheck.Checked = IniDocs.BoolValue("random_no_tree_default");
        _randArcheryCheck.Checked = IniDocs.BoolValue("random_archery_bow");
        _mageWandZeroCheck.Checked = IniDocs.BoolValue("wand_zero_charges");
        _randHealthCheck.Checked = IniDocs.BoolValue("random_health");
        _randTreeCountNum.Value = ClampNum(IniDocs.IntValue("random_tree_count", 4), _randTreeCountNum);
        _midgameSpecCheck.Checked = IniDocs.BoolValue("allow_midgame_spectators");
        _lobbyJoinableCheck.Checked = IniDocs.BoolValue("keep_lobby_joinable");
        _hostSyncCheck.Checked = IniDocs.BoolValue("host_settings_sync", true);
        _mageWandTreeCheck.Checked = IniDocs.BoolValue("wand_skill_tree");
        _mageRandomWandCheck.Checked = IniDocs.BoolValue("random_wand_weapon");
        _debugCheck.Checked = IniDocs.BoolValue("test_warp");
        _warpCombo.SelectedIndex = Math.Clamp(IniDocs.IntValue("warp_floor", 0), 0, _warpCombo.Items.Count - 1);
        _warpLabel.Visible = _warpCombo.Visible = _debugCheck.Checked;
        _loading = false;
        UpdateDirty();
        SetStatus("Reset to defaults. Click Update Settings / Install to apply.", Muted);
    }

    static decimal ClampNum(int v, NumericUpDown n) => Math.Clamp(v, (int)n.Minimum, (int)n.Maximum);

    // Set a tooltip, word-wrapped with real line breaks so it shows as a tidy block instead of one
    // huge line. (Native tooltips position themselves correctly; owner-draw resizing does not.)
    void Tip(Control c, string text) => _tip.SetToolTip(c, Wrap(text, 56));
    static string Wrap(string text, int maxChars)
    {
        var sb = new System.Text.StringBuilder();
        int lineLen = 0;
        foreach (var word in text.Split(' '))
        {
            if (lineLen > 0 && lineLen + 1 + word.Length > maxChars) { sb.Append("\r\n"); lineLen = 0; }
            else if (lineLen > 0) { sb.Append(' '); lineLen++; }
            sb.Append(word);
            lineLen += word.Length;
        }
        return sb.ToString();
    }

    CheckBox MakeCheck(Spell sp, int x, int y)
    {
        var cb = new CheckBox {
            Text = IniDocs.Title(sp.Key), Tag = sp, AutoSize = true, ForeColor = Fg,
            Location = new Point(x, y), FlatStyle = FlatStyle.Flat, Checked = sp.Default };
        Tip(cb, IniDocs.Desc(sp.Key));
        _checks[sp.Key] = cb;
        return cb;
    }

    Button MakeButton(string t, int x, int y, int w, bool secondary = false)
    {
        var b = new Button {
            Text = t, Location = new Point(x, y), Size = new Size(w, 34),
            FlatStyle = FlatStyle.Flat, ForeColor = Color.White,
            BackColor = secondary ? Panel : Accent, Cursor = Cursors.Hand };
        b.FlatAppearance.BorderColor = secondary ? Color.FromArgb(0x44, 0x44, 0x52) : Accent;
        b.FlatAppearance.MouseOverBackColor = secondary ? Color.FromArgb(0x35, 0x35, 0x42) : Accent2;
        return b;
    }

    // ---- logic ----
    void Detect()
    {
        var dir = SteamLocator.FindVagante();
        if (dir != null) SetGame(dir);
        else { _pathBox.Text = "(not found - click Browse…)"; SetStatus("Couldn't auto-detect Vagante. Use Browse to point at the game folder.", Warn); RefreshButtons(); }
    }

    void SetGame(string dir)
    {
        _loading = true;
        _inst = new ModInstaller(dir);
        _pathBox.Text = dir;
        var pool = _inst.ReadPool();
        foreach (var (k, cb) in _checks) cb.Checked = pool.TryGetValue(k, out var v) ? v : ((Spell)cb.Tag!).Default;
        // Wisp Curse selectable background is hidden and forced off; never restore it from the ini.
        _wispCheck.Checked = false;
        _loopCheck.Checked = _inst.ReadLoop();
        _startDiffNum.Value = Math.Clamp(_inst.ReadStartDifficulty(), (int)_startDiffNum.Minimum, (int)_startDiffNum.Maximum);
        _forceWispCheck.Checked = _inst.ReadForceWispCurse();
        _fairyCombo.SelectedIndex = Math.Clamp(_inst.ReadFairyMode(), 0, _fairyCombo.Items.Count - 1);
        _fairyStackCheck.Checked = _inst.ReadFairyStack();
        _fairyHealNum.Value = Math.Clamp(_inst.ReadFairyHeal(), (int)_fairyHealNum.Minimum, (int)_fairyHealNum.Maximum);
        _mageWandTreeCheck.Checked = _inst.ReadMageWandTree();
        _mageRandomWandCheck.Checked = _inst.ReadMageRandomWand();
        _mageWandZeroCheck.Checked = _inst.ReadMageWandZero();
        _loopStartCombo.SelectedIndex = Math.Clamp(_inst.ReadLoopStartFloor(), 0, _loopStartCombo.Items.Count - 1);
        _scalingStartsNum.Value = Math.Clamp(_inst.ReadScalingStartsAt(), (int)_scalingStartsNum.Minimum, (int)_scalingStartsNum.Maximum);
        _harderByNum.Value = (decimal)Math.Clamp(_inst.ReadHarderByPct(), (double)_harderByNum.Minimum, (double)_harderByNum.Maximum);
        _spawnStartsNum.Value = Math.Clamp(_inst.ReadSpawnIncreaseStartsAt(), (int)_spawnStartsNum.Minimum, (int)_spawnStartsNum.Maximum);
        _spawnPctNum.Value = Math.Clamp(_inst.ReadSpawnIncreasePct(), (int)_spawnPctNum.Minimum, (int)_spawnPctNum.Maximum);
        _spawnMaxNum.Value = Math.Clamp(_inst.ReadSpawnIncreaseMaxPct(), (int)_spawnMaxNum.Minimum, (int)_spawnMaxNum.Maximum);
        _spawnSpreadNum.Value = Math.Clamp(_inst.ReadSpawnSpreadPct(), (int)_spawnSpreadNum.Minimum, (int)_spawnSpreadNum.Maximum);
        _spawnSpreadStartsNum.Value = Math.Clamp(_inst.ReadSpawnSpreadStartsAt(), (int)_spawnSpreadStartsNum.Minimum, (int)_spawnSpreadStartsNum.Maximum);
        _spawnSpreadMaxNum.Value = Math.Clamp(_inst.ReadSpawnSpreadMaxPct(), (int)_spawnSpreadMaxNum.Minimum, (int)_spawnSpreadMaxNum.Maximum);
        _cursedBombCheck.Checked = _inst.ReadCursedBomb();
        _randWeaponCheck.Checked = _inst.ReadRandomMatchWeapon();
        _randNoTreeCheck.Checked = _inst.ReadRandomNoTreeDefault();
        _randArcheryCheck.Checked = _inst.ReadRandomArcheryBow();
        _randHealthCheck.Checked = _inst.ReadRandomHealth();
        _randTreeCountNum.Value = Math.Clamp(_inst.ReadRandomTreeCount(), (int)_randTreeCountNum.Minimum, (int)_randTreeCountNum.Maximum);
        _midgameSpecCheck.Checked = _inst.ReadAllowMidgameSpectators();
        _lobbyJoinableCheck.Checked = _inst.ReadKeepLobbyJoinable();
        _hostSyncCheck.Checked = _inst.ReadHostSettingsSync();
        _debugCheck.Checked = _inst.ReadDebugWarp();
        _warpCombo.SelectedIndex = Math.Clamp(_inst.ReadWarpFloor(), 0, _warpCombo.Items.Count - 1);
        _warpLabel.Visible = _warpCombo.Visible = _debugCheck.Checked;
        RefreshButtons();
        SetStatus(_inst.IsInstalled ? "Mod is installed. Toggle spells, then click 'Updates Settings'."
                                    : "Mod not installed yet. Pick your spells and click Install / Update.",
                  _inst.IsInstalled ? Ok : Muted);
        _loading = false;
        MarkSaved();   // the UI now mirrors the ini -> no unsaved changes
    }

    void Browse()
    {
        using var d = new FolderBrowserDialog { Description = "Select the Vagante game folder (contains vagante.exe)" };
        if (d.SelectedPath == "" && _inst != null) d.SelectedPath = _inst.GameDir;
        if (d.ShowDialog() != DialogResult.OK) return;
        if (!File.Exists(Path.Combine(d.SelectedPath, "vagante.exe"))) { SetStatus("That folder has no vagante.exe.", Warn); return; }
        SetGame(d.SelectedPath);
    }

    Dictionary<string, bool> PoolFromUi() => _checks.ToDictionary(kv => kv.Key, kv => kv.Value.Checked);

    void DoInstall()
    {
        if (_inst == null) return;
        if (ModInstaller.IsGameRunning) { SetStatus("Close Vagante first, then Install.", Warn); return; }
        try { _inst.Install(PoolFromUi(), _wispCheck.Checked, _loopCheck.Checked, _forceWispCheck.Checked, _fairyCombo.SelectedIndex, _fairyStackCheck.Checked, (int)_fairyHealNum.Value, _debugCheck.Checked, _warpCombo.SelectedIndex, _loopStartCombo.SelectedIndex, (int)_startDiffNum.Value, _mageWandTreeCheck.Checked, _mageRandomWandCheck.Checked, _mageWandZeroCheck.Checked, (double)_harderByNum.Value, (int)_scalingStartsNum.Value, (int)_spawnPctNum.Value, (int)_spawnStartsNum.Value, (int)_spawnMaxNum.Value, (int)_spawnSpreadNum.Value, (int)_spawnSpreadStartsNum.Value, (int)_spawnSpreadMaxNum.Value, _cursedBombCheck.Checked, _randWeaponCheck.Checked, _randNoTreeCheck.Checked, _randArcheryCheck.Checked, _randHealthCheck.Checked, (int)_randTreeCountNum.Value, _midgameSpecCheck.Checked, _lobbyJoinableCheck.Checked, _hostSyncCheck.Checked); SetStatus("Installed. Launch Vagante - changes apply as new spellbooks generate.", Ok); MarkSaved(); }
        catch (Exception ex) { SetStatus("Install failed: " + ex.Message, Warn); }
        RefreshButtons();
    }

    void DoSave()
    {
        if (_inst == null) return;
        try
        {
            _inst.WriteIni(PoolFromUi(), _wispCheck.Checked, _loopCheck.Checked, _forceWispCheck.Checked, _fairyCombo.SelectedIndex, _fairyStackCheck.Checked, (int)_fairyHealNum.Value, _debugCheck.Checked, _warpCombo.SelectedIndex, _loopStartCombo.SelectedIndex, (int)_startDiffNum.Value, _mageWandTreeCheck.Checked, _mageRandomWandCheck.Checked, _mageWandZeroCheck.Checked, (double)_harderByNum.Value, (int)_scalingStartsNum.Value, (int)_spawnPctNum.Value, (int)_spawnStartsNum.Value, (int)_spawnMaxNum.Value, (int)_spawnSpreadNum.Value, (int)_spawnSpreadStartsNum.Value, (int)_spawnSpreadMaxNum.Value, _cursedBombCheck.Checked, _randWeaponCheck.Checked, _randNoTreeCheck.Checked, _randArcheryCheck.Checked, _randHealthCheck.Checked, (int)_randTreeCountNum.Value, _midgameSpecCheck.Checked, _lobbyJoinableCheck.Checked, _hostSyncCheck.Checked);
            SetStatus(ModInstaller.IsGameRunning
                ? "Saved. Already-running game picks it up on the next room/level."
                : "Saved.", Ok);
            MarkSaved();
        }
        catch (Exception ex) { SetStatus("Save failed: " + ex.Message, Warn); }
    }

    void DoUninstall()
    {
        if (_inst == null) return;
        if (ModInstaller.IsGameRunning) { SetStatus("Close Vagante first, then Uninstall.", Warn); return; }
        try { _inst.Uninstall(); SetStatus("Uninstalled - original game restored.", Ok); }
        catch (Exception ex) { SetStatus("Uninstall failed: " + ex.Message, Warn); }
        RefreshButtons();
    }

    void RefreshButtons()
    {
        bool has = _inst != null;
        _btnInstall.Enabled = has;
        _btnSave.Enabled = has && _inst!.IsInstalled;
        _btnUninstall.Enabled = has && _inst!.IsInstalled;
    }

    void SetStatus(string msg, Color c) { _status.ForeColor = c; _status.Text = msg; }

    // ---- "unsaved changes" tracking ----
    // Recursively subscribe to every input control so any edit re-evaluates the dirty state.
    void WireDirtyTracking(Control parent)
    {
        foreach (Control c in parent.Controls)
        {
            switch (c)
            {
                case CheckBox cb:      cb.CheckedChanged      += (_, _) => OnUiChanged(); break;
                case NumericUpDown n:  n.ValueChanged         += (_, _) => OnUiChanged(); break;
                case ComboBox cmb:     cmb.SelectedIndexChanged += (_, _) => OnUiChanged(); break;
            }
            if (c.HasChildren) WireDirtyTracking(c);
        }
    }

    void OnUiChanged() { if (!_loading) UpdateDirty(); }

    // A fingerprint of every setting that gets written to the ini, in WriteIni's argument order.
    string CurrentSignature()
    {
        var sb = new System.Text.StringBuilder();
        foreach (var sp in Spells.All)
            sb.Append(_checks.TryGetValue(sp.Key, out var cb) && cb.Checked ? '1' : '0');
        sb.Append('|').Append(_wispCheck.Checked ? 1 : 0)
          .Append('|').Append(_loopCheck.Checked ? 1 : 0)
          .Append('|').Append(_forceWispCheck.Checked ? 1 : 0)
          .Append('|').Append(_fairyCombo.SelectedIndex)
          .Append('|').Append(_fairyStackCheck.Checked ? 1 : 0)
          .Append('|').Append((int)_fairyHealNum.Value)
          .Append('|').Append(_debugCheck.Checked ? 1 : 0)
          .Append('|').Append(_warpCombo.SelectedIndex)
          .Append('|').Append(_loopStartCombo.SelectedIndex)
          .Append('|').Append((int)_startDiffNum.Value)
          .Append('|').Append(_mageWandTreeCheck.Checked ? 1 : 0)
          .Append('|').Append(_mageRandomWandCheck.Checked ? 1 : 0)
          .Append('|').Append(_mageWandZeroCheck.Checked ? 1 : 0)
          .Append('|').Append(((double)_harderByNum.Value).ToString(System.Globalization.CultureInfo.InvariantCulture))
          .Append('|').Append((int)_scalingStartsNum.Value)
          .Append('|').Append((int)_spawnPctNum.Value)
          .Append('|').Append((int)_spawnStartsNum.Value)
          .Append('|').Append((int)_spawnMaxNum.Value)
          .Append('|').Append((int)_spawnSpreadNum.Value)
          .Append('|').Append((int)_spawnSpreadStartsNum.Value)
          .Append('|').Append((int)_spawnSpreadMaxNum.Value)
          .Append('|').Append(_cursedBombCheck.Checked ? 1 : 0)
          .Append('|').Append(_randWeaponCheck.Checked ? 1 : 0)
          .Append('|').Append(_randNoTreeCheck.Checked ? 1 : 0)
          .Append('|').Append(_randArcheryCheck.Checked ? 1 : 0)
          .Append('|').Append(_randHealthCheck.Checked ? 1 : 0)
          .Append('|').Append((int)_randTreeCountNum.Value)
          .Append('|').Append(_midgameSpecCheck.Checked ? 1 : 0)
          .Append('|').Append(_lobbyJoinableCheck.Checked ? 1 : 0)
          .Append('|').Append(_hostSyncCheck.Checked ? 1 : 0);
        return sb.ToString();
    }

    // Records the current UI as the "saved" baseline (called after a load or a successful write).
    void MarkSaved() { _savedSig = CurrentSignature(); UpdateDirty(); }

    void UpdateDirty()
    {
        bool dirty = _inst != null && CurrentSignature() != _savedSig;
        _dirtyLbl.Text = _inst != null && !_inst.IsInstalled
            ? "● Unsaved changes - click Install to apply them to Vagante."
            : "● Unsaved changes - click Update Settings to apply them to Vagante.";
        _dirtyLbl.Visible = dirty;
        // Highlight the button that actually writes the changes so it's obvious what to click.
        Button live = (_inst != null && _inst.IsInstalled) ? _btnSave : _btnInstall;
        foreach (var b in new[] { _btnSave, _btnInstall })
        {
            bool hot = dirty && b == live;
            b.BackColor = hot ? Warn : (b == _btnSave ? Accent : Panel);
            b.FlatAppearance.BorderColor = hot ? Warn : (b == _btnSave ? Accent : Color.FromArgb(0x44, 0x44, 0x52));
        }
    }
}
