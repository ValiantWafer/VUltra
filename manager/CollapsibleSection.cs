using System.ComponentModel;

namespace VUltra;

/// <summary>
/// A section with a clickable header (▼ / ▶) that shows or hides its Body. Drop controls into
/// <see cref="Body"/> using local coordinates; the Body auto-sizes to fit them and the whole
/// section grows/shrinks as it is expanded/collapsed. Meant to sit in a top-down FlowLayoutPanel,
/// which re-flows the sections below whenever one toggles.
/// </summary>
public sealed class CollapsibleSection : Panel
{
    const int HeaderH = 30;
    readonly Label _header;
    readonly string _title;
    bool _expanded = true;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Panel Body { get; }

    public CollapsibleSection(string title, Color headerColor, int width)
    {
        _title = title;
        Width = width;
        Margin = new Padding(0, 0, 0, 6);
        _header = new Label {
            Location = new Point(0, 0), Size = new Size(width, HeaderH),
            ForeColor = headerColor, Font = new Font("Segoe UI Semibold", 9f),
            TextAlign = ContentAlignment.MiddleLeft, Cursor = Cursors.Hand };
        Body = new Panel {
            Location = new Point(0, HeaderH), Width = width,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Padding = new Padding(0, 0, 0, 8) };
        Controls.Add(_header);
        Controls.Add(Body);
        _header.Click += (_, _) => Expanded = !Expanded;
        Body.Resize += (_, _) => Relayout();
        UpdateHeader();
        Relayout();
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Expanded
    {
        get => _expanded;
        set { _expanded = value; Body.Visible = value; UpdateHeader(); Relayout(); }
    }

    void UpdateHeader() => _header.Text = (_expanded ? "▼  " : "▶  ") + _title;
    // Use the header's live height, not HeaderH: DPI scaling enlarges the header on load.
    void Relayout()
    {
        Body.Top = _header.Height;
        Height = _header.Height + (_expanded ? Body.Height : 0);
    }
}
