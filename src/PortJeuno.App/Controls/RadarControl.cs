using System;
using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace PortJeuno.App.Controls;

/// <summary>One thing to draw on the radar, in world coordinates.</summary>
public readonly record struct RadarBlip(float X, float Depth, string Label, bool IsPlayer, bool IsSelf);

/// <summary>
/// A top-down map drawn from the zone's navmesh, with entities on top.
///
/// Drawn directly rather than assembled from an ItemsControl over a Canvas.
/// That approach needs `Canvas.Left`/`Top` set on the generated container
/// rather than the templated child, and brushes bound as `IBrush` rather than
/// colour strings - both of which fail silently and leave an empty panel with
/// no error anywhere. Rendering is simpler to reason about and impossible to
/// get subtly wrong in that way.
///
/// Coordinates: screen x follows world x, screen y follows world *depth*.
/// Height is deliberately dropped - this is a plan view.
/// </summary>
public sealed class RadarControl : Control
{
    private IReadOnlyList<IReadOnlyList<(float X, float Depth)>> _polygons = [];
    private IReadOnlyList<RadarBlip> _blips = [];
    private IReadOnlyList<(float X, float Depth)> _path = [];
    private float _centreX;
    private float _centreDepth;

    /// <summary>World units from the centre to the edge.</summary>
    public float Range { get; set; } = 50f;

    /// <summary>Raised when the user clicks the map, with the world position they clicked.</summary>
    public event Action<float, float>? Clicked;

    private static readonly IBrush Background = new SolidColorBrush(Color.Parse("#0E0E10"));
    private static readonly IBrush Walkable = new SolidColorBrush(Color.Parse("#1E2A33"));
    private static readonly IPen WalkableEdge = new Pen(new SolidColorBrush(Color.Parse("#2C3E4A")), 1);
    private static readonly IPen Grid = new Pen(new SolidColorBrush(Color.Parse("#1A1A1D")), 1);
    private static readonly IPen PathPen = new Pen(new SolidColorBrush(Color.Parse("#4CAF50")), 2);
    private static readonly IBrush SelfBrush = new SolidColorBrush(Color.Parse("#4FC3F7"));
    private static readonly IBrush PlayerBrush = new SolidColorBrush(Color.Parse("#FFD54F"));
    private static readonly IBrush NpcBrush = new SolidColorBrush(Color.Parse("#7E8A93"));
    private static readonly IBrush LabelBrush = new SolidColorBrush(Color.Parse("#DDE3E7"));

    public RadarControl()
    {
        PointerPressed += OnPointerPressed;
    }

    /// <summary>Replaces everything drawn and asks for a repaint.</summary>
    public void Update(
        float centreX,
        float centreDepth,
        IReadOnlyList<IReadOnlyList<(float X, float Depth)>> polygons,
        IReadOnlyList<RadarBlip> blips,
        IReadOnlyList<(float X, float Depth)> path)
    {
        _centreX = centreX;
        _centreDepth = centreDepth;
        _polygons = polygons;
        _blips = blips;
        _path = path;
        InvalidateVisual();
    }

    private Point ToScreen(float worldX, float worldDepth, double half, double scale) =>
        new(half + ((worldX - _centreX) * scale), half + ((worldDepth - _centreDepth) * scale));

    private void OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        double half = Math.Min(Bounds.Width, Bounds.Height) / 2;
        if (half <= 0)
        {
            return;
        }

        double scale = half / Range;
        Point p = e.GetPosition(this);

        Clicked?.Invoke(
            _centreX + (float)((p.X - half) / scale),
            _centreDepth + (float)((p.Y - half) / scale));
    }

    public override void Render(DrawingContext context)
    {
        double size = Math.Min(Bounds.Width, Bounds.Height);
        double half = size / 2;
        if (half <= 0)
        {
            return;
        }

        double scale = half / Range;

        context.FillRectangle(Background, new Rect(0, 0, size, size));

        // Range rings, every 25 world units, as a sense of scale.
        for (float r = 25; r <= Range; r += 25)
        {
            context.DrawEllipse(null, Grid, new Point(half, half), r * scale, r * scale);
        }

        // The walkable surface. Anything not drawn here is somewhere the
        // character cannot stand - which makes the map double as a picture of
        // where collision will stop you.
        foreach (IReadOnlyList<(float X, float Depth)> polygon in _polygons)
        {
            if (polygon.Count < 3)
            {
                continue;
            }

            var geometry = new StreamGeometry();
            using (StreamGeometryContext g = geometry.Open())
            {
                g.BeginFigure(ToScreen(polygon[0].X, polygon[0].Depth, half, scale), isFilled: true);
                for (int i = 1; i < polygon.Count; i++)
                {
                    g.LineTo(ToScreen(polygon[i].X, polygon[i].Depth, half, scale));
                }
                g.EndFigure(isClosed: true);
            }

            context.DrawGeometry(Walkable, WalkableEdge, geometry);
        }

        // The route we're following, if any.
        for (int i = 1; i < _path.Count; i++)
        {
            context.DrawLine(
                PathPen,
                ToScreen(_path[i - 1].X, _path[i - 1].Depth, half, scale),
                ToScreen(_path[i].X, _path[i].Depth, half, scale));
        }

        foreach (RadarBlip blip in _blips)
        {
            Point p = ToScreen(blip.X, blip.Depth, half, scale);
            if (p.X < 0 || p.Y < 0 || p.X > size || p.Y > size)
            {
                continue;
            }

            IBrush brush = blip.IsSelf ? SelfBrush : blip.IsPlayer ? PlayerBrush : NpcBrush;
            double radius = blip.IsSelf ? 5 : blip.IsPlayer ? 4.5 : 2.5;

            context.DrawEllipse(brush, null, p, radius, radius);

            // Only players get a name; labelling every NPC would be unreadable.
            if (blip.IsPlayer && blip.Label.Length > 0)
            {
                var text = new FormattedText(
                    blip.Label,
                    System.Globalization.CultureInfo.InvariantCulture,
                    FlowDirection.LeftToRight,
                    Typeface.Default,
                    10,
                    LabelBrush);

                context.DrawText(text, new Point(p.X + 6, p.Y - 6));
            }
        }
    }
}
