# LVGL v9.4 Scale Widget for ESPHome

## Overview

The Scale widget is a versatile LVGL v9.4 component for displaying measurement scales in various orientations. It replaces the obsolete Meter widget from LVGL v8.x and provides more flexibility for creating gauges, thermometers, progress indicators, and other measurement displays.

**IMPORTANT**: The Scale widget only displays tick marks and labels (static). For animated gauges, combine Scale + Arc widgets.

## Features

- **Multiple scale modes**: horizontal, vertical, and circular (round inner/outer)
- **Configurable tick marks**: Both major and minor ticks with independent styling
- **Value range**: Configurable minimum and maximum values
- **Colored sections**: Define colored zones for different value ranges (e.g., green/yellow/red zones)
- **Label support**: Automatic labels for major ticks
- **Rotation**: Configurable start angle and angle range for circular scales
- **Parts**: MAIN (background), INDICATOR (major ticks), ITEMS (minor ticks)

## Configuration Schema

### Basic Configuration

```yaml
scale:
  id: my_scale
  mode: ROUND_OUTER          # Scale orientation
  min_value: 0               # Minimum value
  max_value: 100             # Maximum value
  rotation: 135              # Start angle (for round modes)
  angle_range: 270           # Angle coverage (for round modes)
```

### Scale Modes

The `mode` parameter determines the scale orientation:

- **`ROUND_OUTER`**: Circular scale with ticks on the outside (default)
- **`ROUND_INNER`**: Circular scale with ticks on the inside
- **`HORIZONTAL_TOP`**: Horizontal scale with ticks on top
- **`HORIZONTAL_BOTTOM`**: Horizontal scale with ticks on bottom
- **`VERTICAL_LEFT`**: Vertical scale with ticks on left
- **`VERTICAL_RIGHT`**: Vertical scale with ticks on right

### Tick Configuration

```yaml
scale:
  id: my_scale
  ticks:
    count: 21                  # Total number of ticks
    width: 2                   # Line width of minor ticks
    length: 10                 # Length of minor ticks
    color: 0x808080           # Color of minor ticks
    radial_offset: 0          # Offset from scale line
    major:                    # Major tick configuration
      stride: 5               # Every Nth tick is major
      width: 4                # Line width of major ticks
      length: 20              # Length of major ticks
      color: 0xFFFFFF        # Color of major ticks
      radial_offset: 0       # Offset from scale line
      label_show: true       # Show labels on major ticks
      label_gap: 4           # Distance between tick and label
```

### Sections (Colored Zones)

Sections allow you to define colored ranges on the scale:

```yaml
scale:
  id: my_scale
  sections:
    - id: safe_zone
      range_from: 0
      range_to: 60
      color: 0x00FF00         # Green
      width: 6

    - id: warning_zone
      range_from: 60
      range_to: 80
      color: 0xFFFF00         # Yellow
      width: 6

    - id: danger_zone
      range_from: 80
      range_to: 100
      color: 0xFF0000         # Red
      width: 6
```

## Creating Animated Gauges (Scale + Arc)

The Scale widget is static - it only displays ticks and labels. To create an animated gauge:

1. Use **Scale** for tick marks and labels (static background)
2. Use **Arc** overlaid on Scale for the animated indicator
3. Update the Arc value to animate the gauge

### Example: Animated Speedometer

```yaml
globals:
  - id: speed_value
    type: int
    initial_value: "0"

interval:
  - interval: 50ms
    then:
      - lambda: |-
          static int direction = 1;
          id(speed_value) += direction * 2;
          if (id(speed_value) >= 200) direction = -1;
          if (id(speed_value) <= 0) direction = 1;
      - lvgl.arc.update:
          id: speed_arc
          value: !lambda 'return id(speed_value);'
          animated: true
      - lvgl.label.update:
          id: speed_label
          text:
            format: "%d"
            args: [id(speed_value)]

lvgl:
  pages:
    - widgets:
        # Scale for tick marks and labels (static)
        - scale:
            id: speed_scale
            align: CENTER
            width: 280
            height: 280
            mode: ROUND_OUTER
            min_value: 0
            max_value: 200
            rotation: 135
            angle_range: 270
            ticks:
              count: 21
              width: 2
              length: 10
              color: 0x404040
              major:
                stride: 2
                width: 4
                length: 20
                color: 0xFFFFFF
                label_show: true
                label_gap: 12
            sections:
              - id: speed_safe
                range_from: 0
                range_to: 120
                color: 0x00FF00
                width: 6
              - id: speed_caution
                range_from: 120
                range_to: 160
                color: 0xFFFF00
                width: 6
              - id: speed_danger
                range_from: 160
                range_to: 200
                color: 0xFF0000
                width: 6

        # Arc for animated indicator (overlay on scale)
        - arc:
            id: speed_arc
            align: CENTER
            width: 220
            height: 220
            min_value: 0
            max_value: 200
            value: 0
            rotation: 135        # Same as scale
            start_angle: 0
            end_angle: 270       # Same as scale angle_range
            mode: NORMAL
            adjustable: false
            indicator:
              arc_color: 0x00FF00
              arc_width: 20
            main:
              arc_color: 0x1a1a2e
              arc_width: 20

        # Value display
        - label:
            id: speed_label
            align: CENTER
            y: 20
            text: "0"
            text_color: 0x00FF00
            text_font: montserrat_36

        - label:
            align: CENTER
            y: 55
            text: "km/h"
            text_color: 0x808080
```

### Key Points for Scale + Arc Alignment

1. **Same rotation**: Arc and Scale must have the same `rotation` value
2. **Same angle range**: Arc's `end_angle` should match Scale's `angle_range`
3. **Same min/max**: Use identical `min_value` and `max_value`
4. **Arc smaller**: Arc should be slightly smaller than Scale to fit inside
5. **Same alignment**: Use `align: CENTER` for both (or matching x/y coordinates)

## Parts and Styling

The scale widget has three main parts that can be styled independently:

### MAIN Part
The background and main scale line:

```yaml
scale:
  id: my_scale
  main:
    bg_color: 0x000000
    bg_opa: 0.5
    arc_width: 2              # Scale arc line width (for round modes)
    arc_color: 0xCCCCCC       # Scale arc line color
```

### INDICATOR Part
Major ticks and their labels:

```yaml
scale:
  id: my_scale
  indicator:
    line_color: 0xFFFFFF      # Major tick color
    line_width: 4             # Major tick width
    text_color: 0xFFFFFF      # Label text color
    text_font: montserrat_14  # Label font
```

### ITEMS Part
Minor ticks:

```yaml
scale:
  id: my_scale
  items:
    line_color: 0x808080      # Minor tick color
    line_width: 2             # Minor tick width
```

## Usage Examples

### Example 1: Temperature Gauge with Sensor

```yaml
sensor:
  - platform: homeassistant
    id: temperature
    entity_id: sensor.temperature
    on_value:
      then:
        - lvgl.arc.update:
            id: temp_arc
            value: !lambda 'return (int)id(temperature).state;'
            animated: true
        - lvgl.label.update:
            id: temp_label
            text:
              format: "%.1f°C"
              args: [id(temperature).state]

lvgl:
  pages:
    - widgets:
        - scale:
            id: temp_scale
            x: 50
            y: 50
            width: 200
            height: 200
            mode: ROUND_OUTER
            min_value: -10
            max_value: 40
            rotation: 135
            angle_range: 270
            ticks:
              count: 11
              width: 2
              length: 8
              color: 0x404040
              major:
                stride: 2
                width: 3
                length: 15
                color: 0xFFFFFF
                label_show: true
            sections:
              - id: temp_cold
                range_from: -10
                range_to: 15
                color: 0x00BFFF
                width: 4
              - id: temp_comfort
                range_from: 15
                range_to: 26
                color: 0x00FF00
                width: 4
              - id: temp_warm
                range_from: 26
                range_to: 40
                color: 0xFF4500
                width: 4

        - arc:
            id: temp_arc
            x: 75
            y: 75
            width: 150
            height: 150
            min_value: -10
            max_value: 40
            value: 20
            rotation: 135
            start_angle: 0
            end_angle: 270
            mode: NORMAL
            adjustable: false
            indicator:
              arc_color: 0x00FF00
              arc_width: 12
            main:
              arc_color: 0x1a1a2e
              arc_width: 12

        - label:
            id: temp_label
            x: 100
            y: 145
            width: 100
            text_align: CENTER
            text: "--.-°C"
            text_color: 0x00FF00
```

### Example 2: Vertical Scale (Thermometer Style)

```yaml
scale:
  id: thermometer
  x: 50
  y: 50
  width: 80
  height: 200
  mode: VERTICAL_LEFT
  min_value: -20
  max_value: 50
  ticks:
    count: 15
    width: 2
    length: 8
    color: 0x808080
    major:
      stride: 3
      width: 3
      length: 12
      color: 0x000000
      label_show: true
      label_gap: 6
  sections:
    - id: freezing
      range_from: -20
      range_to: 0
      color: 0x0080FF
      width: 5
    - id: normal
      range_from: 0
      range_to: 25
      color: 0x00FF00
      width: 5
    - id: hot
      range_from: 25
      range_to: 50
      color: 0xFF0000
      width: 5
```

### Example 3: Horizontal Progress Scale

```yaml
scale:
  id: progress_scale
  x: 50
  y: 150
  width: 400
  height: 60
  mode: HORIZONTAL_TOP
  min_value: 0
  max_value: 100
  ticks:
    count: 11
    width: 2
    length: 10
    color: 0x404040
    major:
      stride: 2
      width: 3
      length: 18
      color: 0xFFFFFF
      label_show: true
  sections:
    - id: low_section
      range_from: 0
      range_to: 33
      color: 0x0088FF
      width: 4
    - id: mid_section
      range_from: 33
      range_to: 66
      color: 0x00FF88
      width: 4
    - id: high_section
      range_from: 66
      range_to: 100
      color: 0xFF8800
      width: 4
```

## Automation Actions

### Update Scale Configuration

```yaml
lvgl.scale.update:
  id: my_scale
  mode: ROUND_OUTER
  min_value: 0
  max_value: 300
  rotation: 90
  angle_range: 180
```

### Update Section

```yaml
lvgl.scale.section.update:
  id: danger_zone
  range_from: 150
  range_to: 200
  color: 0xFF0000
  width: 8
```

## Migration from Meter Widget

If you're migrating from the LVGL v8.x meter widget:

| Meter (v8.x) | Scale (v9.4) |
|--------------|--------------|
| `lv_meter_create()` | `lv_scale_create()` + `lv_arc_create()` |
| `lv_meter_set_scale_ticks()` | `ticks.count` configuration |
| `lv_meter_set_scale_major_ticks()` | `ticks.major.stride` configuration |
| `lv_meter_add_arc()` | `sections` configuration |
| `lv_meter_add_needle_line()` | Use Arc widget with value update |
| Automatic value indication | Manually update Arc value |

## API Reference

### LVGL v9.4 Functions Used

The scale widget implementation uses these LVGL v9.4 API functions:

- `lv_scale_create(parent)` - Create scale widget
- `lv_scale_set_mode(scale, mode)` - Set scale orientation
- `lv_scale_set_range(scale, min, max)` - Set value range
- `lv_scale_set_rotation(scale, angle)` - Set rotation angle
- `lv_scale_set_angle_range(scale, range)` - Set angle coverage
- `lv_scale_set_total_tick_count(scale, count)` - Set tick count
- `lv_scale_set_major_tick_every(scale, stride)` - Set major tick frequency
- `lv_scale_set_label_show(scale, show)` - Enable/disable labels
- `lv_scale_add_section(scale)` - Add colored section
- `lv_scale_section_set_range(section, start, end)` - Set section range
- `lv_scale_section_set_style(section, part, style)` - Apply style to section

## Tips and Best Practices

1. **Tick Count**: Choose tick counts that divide evenly into your value range for clean labeling
2. **Major Tick Stride**: Typical values are 2, 3, 5, or 10 depending on total tick count
3. **Angle Range**: For circular scales, 270° provides good visibility (avoids bottom area)
4. **Rotation**: 135° rotation (starting at bottom-left) is common for speedometer-style gauges
5. **Section Width**: Make section lines wider than tick lines for better visibility
6. **Label Gap**: Adjust based on your font size to prevent label overlap with ticks
7. **Color Contrast**: Ensure good contrast between ticks, labels, and background
8. **Arc Alignment**: When using Scale + Arc, keep rotation and angle_range identical

## Troubleshooting

### Scale Not Animating
- Scale widget is static - it only shows ticks/labels
- Use Arc widget overlaid on Scale for animated indicator
- Update the Arc value to animate

### Labels Not Showing
- Ensure `label_show: true` in major tick configuration
- Check that font is properly loaded
- Verify label_gap is appropriate for your scale size

### Sections Not Visible
- Ensure section width is greater than 0
- Check color is valid hex (0xRRGGBB)
- Verify section ranges are within min/max values

### Arc Not Aligned with Scale
- Arc and Scale must have same `rotation` value
- Arc's `end_angle` should match Scale's `angle_range`
- Use same `min_value` and `max_value` for both
- Arc should be smaller than Scale (to fit inside)

### Ticks Overlapping
- Reduce tick count
- Increase scale size
- Adjust tick length

### Circular Scale Cut Off
- Ensure width and height are equal for round modes
- Increase widget size
- Adjust padding and margins

## References

- [LVGL v9.4 Scale Documentation](https://docs.lvgl.io/9.4/details/widgets/scale.html)
- [ESPHome LVGL Component](https://esphome.io/components/lvgl/index.html)
- See `scale_example.yaml` for complete working examples
