# LVGL Chart Widget Implementation for ESPHome

## Overview

This implementation provides full support for the LVGL v9.4 Chart widget (`lv_chart`) in ESPHome. The chart widget displays data visualizations including line charts, bar charts, and scatter plots with support for multiple data series.

## Features

- **Chart Types**: LINE, BAR, SCATTER, NONE
- **Update Modes**: SHIFT (linear buffer), CIRCULAR (ECG-style)
- **Multiple Axes**: PRIMARY_Y, SECONDARY_Y, PRIMARY_X, SECONDARY_X
- **Multiple Series**: Each with its own color and axis
- **Interactive**: Detect pressed points with `on_value`
- **Dynamic Colors**: Change series colors at runtime
- **Faded Area**: Gradient effects under line charts
- **Animations**: Update points dynamically with intervals

## Actions Available

| Action | Description |
|--------|-------------|
| `lvgl.chart.set_next_value` | Add a point using SHIFT/CIRCULAR mode |
| `lvgl.chart.set_value_by_id` | Set a specific point by index |
| `lvgl.chart.set_value_by_id2` | Set X/Y values for scatter charts |
| `lvgl.chart.set_series_color` | Change series color dynamically |
| `lvgl.chart.refresh` | Refresh chart display |

## Configuration Schema

### Chart Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | enum | LINE | Chart type: LINE, BAR, SCATTER, NONE |
| `point_count` | int | 10 | Number of data points |
| `update_mode` | enum | SHIFT | SHIFT or CIRCULAR |
| `div_line_count_hor` | int | 3 | Horizontal grid lines |
| `div_line_count_ver` | int | 5 | Vertical grid lines |
| `series` | list | - | List of data series |
| `axis_primary_y` | dict | - | Primary Y axis config |
| `axis_secondary_y` | dict | - | Secondary Y axis config |
| `axis_primary_x` | dict | - | Primary X axis config |
| `axis_secondary_x` | dict | - | Secondary X axis config |

### Series Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | id | required | Series identifier |
| `color` | color | red | Series color |
| `axis` | enum | PRIMARY_Y | Which axis to use |
| `points` | list | - | Initial Y values (LINE/BAR) |
| `x_points` | list | - | Initial X values (SCATTER) |
| `y_points` | list | - | Initial Y values (SCATTER) |

### Axis Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `min_value` | int | Minimum axis value |
| `max_value` | int | Maximum axis value |

### Styling Parts

| Part | Description |
|------|-------------|
| `main` | Chart background, border, grid lines |
| `items` | Data series (lines/bars) |
| `indicator` | Point markers on line/scatter |
| `cursor` | Cursor visualization |
| `scrollbar` | Zoom navigation |

## Usage Examples

### 1. Basic Line Chart with Sensor

```yaml
sensor:
  - platform: homeassistant
    id: temperature
    entity_id: sensor.temperature
    on_value:
      then:
        - lvgl.chart.set_next_value:
            id: temp_chart
            series_id: temp_series
            value: !lambda 'return (int)id(temperature).state;'

lvgl:
  pages:
    - widgets:
        - chart:
            id: temp_chart
            x: 20
            y: 20
            width: 300
            height: 150
            type: LINE
            point_count: 24
            update_mode: SHIFT
            axis_primary_y:
              min_value: 0
              max_value: 40
            series:
              - id: temp_series
                color: 0xFF0000
```

### 2. Dual-Axis Chart (Temperature + Humidity)

```yaml
sensor:
  - platform: homeassistant
    id: weather_temp
    entity_id: weather.home
    attribute: temperature
    on_value:
      then:
        - lvgl.label.update:
            id: temp_label
            text:
              format: "%.1f°C"
              args: [id(weather_temp).state]
        - lvgl.chart.set_next_value:
            id: weather_chart
            series_id: temp_series
            value: !lambda 'return (int)id(weather_temp).state;'

  - platform: homeassistant
    id: weather_humidity
    entity_id: weather.home
    attribute: humidity
    on_value:
      then:
        - lvgl.label.update:
            id: humidity_label
            text:
              format: "%.0f%%"
              args: [id(weather_humidity).state]
        - lvgl.chart.set_next_value:
            id: weather_chart
            series_id: humidity_series
            value: !lambda 'return (int)id(weather_humidity).state;'

lvgl:
  pages:
    - widgets:
        # Current value labels
        - label:
            id: temp_label
            x: 20
            y: 10
            text: "--.-°C"
            text_color: 0xFF0000
        - label:
            id: humidity_label
            x: 120
            y: 10
            text: "--%"
            text_color: 0x0000FF

        # Dual-axis chart
        - chart:
            id: weather_chart
            x: 20
            y: 40
            width: 350
            height: 180
            type: LINE
            point_count: 24
            div_line_count_hor: 4
            div_line_count_ver: 6
            items:
              line_width: 2
            indicator:
              radius: 3
            axis_primary_y:
              min_value: -10
              max_value: 40
            axis_secondary_y:
              min_value: 0
              max_value: 100
            series:
              - id: temp_series
                color: 0xFF0000
                axis: PRIMARY_Y
              - id: humidity_series
                color: 0x0000FF
                axis: SECONDARY_Y
```

### 3. ECG-Style Circular Chart

```yaml
globals:
  - id: ecg_counter
    type: int
    initial_value: "0"

interval:
  - interval: 100ms
    then:
      - lambda: 'id(ecg_counter)++;'
      - lvgl.chart.set_next_value:
          id: ecg_chart
          series_id: ecg_series
          value: !lambda |-
            int val = 50;
            if (id(ecg_counter) % 20 == 0) val = 90;
            else if (id(ecg_counter) % 20 == 1) val = 10;
            else if (id(ecg_counter) % 20 == 5) val = 60;
            else val = 50 + (rand() % 10) - 5;
            return val;

lvgl:
  pages:
    - widgets:
        - chart:
            id: ecg_chart
            x: 20
            y: 20
            width: 400
            height: 200
            type: LINE
            point_count: 150
            update_mode: CIRCULAR
            bg_color: 0x001100
            items:
              line_color: 0x00FF00
              line_width: 2
            indicator:
              radius: 0
            axis_primary_y:
              min_value: 0
              max_value: 100
            series:
              - id: ecg_series
                color: 0x00FF00
```

### 4. Animated Scatter Chart

```yaml
globals:
  - id: angle
    type: float
    initial_value: "0"

interval:
  - interval: 50ms
    then:
      - lambda: |-
          id(angle) += 0.1;
          if (id(angle) > 6.28) id(angle) = 0;
      - lvgl.chart.set_value_by_id2:
          id: scatter_chart
          series_id: scatter_series
          point_index: 0
          x_value: !lambda 'return 50 + (int)(40 * cos(id(angle)));'
          y_value: !lambda 'return 50 + (int)(40 * sin(id(angle)));'

lvgl:
  pages:
    - widgets:
        - chart:
            id: scatter_chart
            x: 50
            y: 50
            width: 250
            height: 250
            type: SCATTER
            point_count: 8
            indicator:
              bg_color: 0xFF6600
              radius: 8
            axis_primary_y:
              min_value: 0
              max_value: 100
            axis_primary_x:
              min_value: 0
              max_value: 100
            series:
              - id: scatter_series
                color: 0xFF6600
                x_points: [50, 85, 85, 50, 15, 15, 50, 50]
                y_points: [10, 30, 70, 90, 70, 30, 50, 50]
```

### 5. Interactive Bar Chart (Detect Clicks)

```yaml
lvgl:
  pages:
    - widgets:
        - label:
            id: clicked_label
            x: 20
            y: 10
            text: "Click a bar..."
            text_color: 0xFFCC00

        - chart:
            id: bar_chart
            x: 20
            y: 40
            width: 350
            height: 200
            type: BAR
            point_count: 12
            clickable: true
            axis_primary_y:
              min_value: 0
              max_value: 500
            series:
              - id: sales_series
                color: 0x4CAF50
                points: [120, 180, 240, 200, 280, 350, 420, 380, 320, 290, 250, 200]
            on_value:
              then:
                - lvgl.label.update:
                    id: clicked_label
                    text:
                      format: "Bar %d clicked!"
                      args: [point_index]
```

### 6. Dynamic Color Change

```yaml
interval:
  - interval: 2s
    then:
      - if:
          condition:
            lambda: 'return id(temperature).state > 25;'
          then:
            - lvgl.chart.set_series_color:
                id: temp_chart
                series_id: temp_series
                series_color: 0xFF0000  # Red when hot
          else:
            - lvgl.chart.set_series_color:
                id: temp_chart
                series_id: temp_series
                series_color: 0x0000FF  # Blue when cold
```

### 7. Faded Area Line Chart

```yaml
lvgl:
  pages:
    - widgets:
        - chart:
            id: faded_chart
            x: 20
            y: 20
            width: 350
            height: 200
            type: LINE
            point_count: 20
            bg_color: 0x0a0a14
            items:
              line_color: 0x00BFFF
              line_width: 3
              bg_color: 0x00BFFF
              bg_opa: 30%
              bg_grad_color: 0x0a0a14
              bg_grad_dir: VER
            indicator:
              bg_color: 0x00BFFF
              radius: 4
            axis_primary_y:
              min_value: 0
              max_value: 100
            series:
              - id: faded_series
                color: 0x00BFFF
                points: [20, 35, 28, 45, 40, 55, 48, 65, 58, 72]
```

## Troubleshooting

### Labels show "--" instead of values

Make sure the sensor is receiving data from Home Assistant. Check:
1. The entity_id is correct
2. Home Assistant API is connected
3. The attribute exists (for weather entities)

```yaml
# Debug: Log sensor values
sensor:
  - platform: homeassistant
    id: my_sensor
    entity_id: sensor.my_entity
    on_value:
      then:
        - logger.log:
            format: "Received value: %.2f"
            args: [id(my_sensor).state]
```

### Chart not updating

1. Ensure `series_id` matches the `id` defined in `series:`
2. Call `lvgl.chart.refresh` if modifying data directly
3. Check that axis range includes your data values

### Points not visible

1. Set `indicator: radius: 4` to show points
2. Ensure `axis_primary_y` range includes your values
3. Check `point_count` is sufficient
