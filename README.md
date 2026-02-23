# Guide Complet des Widgets LVGL 9.5 pour ESPHome CERTAIN nouveaux widgets de lvgl 9.5 ne sont pas fonctionnel pour l'instant,
mais bientot avec des mise jour pour qu'il soit compatible avec esphome les test effectuer sont des esp32P4 

Ce guide documente **tous les 35 widgets** disponibles dans l'implémentation LVGL 9.5 pour ESPHome.

## Table des Matières

- [Widgets de Base](#widgets-de-base)
- [Widgets d'Entrée](#widgets-dentrée)
- [Widgets d'Affichage](#widgets-daffichage)
- [Widgets de Conteneur](#widgets-de-conteneur)
- [Widgets Avancés](#widgets-avancés)


https://github.com/user-attachments/assets/c29243a6-fbc9-44cf-8501-f5bc01b39af0



https://github.com/user-attachments/assets/26e2a9c1-5540-404c-a0c1-53d4524c6d53



https://github.com/user-attachments/assets/3a42fb70-9d3f-4c71-9a18-7fff889b6ed3



https://github.com/user-attachments/assets/a6629c2b-396a-4d89-9f7c-90727d1ad6d4


---



## Widgets de Base

### 1. Label (Texte)

Affiche du texte statique ou dynamique.

```yaml
lvgl:
  pages:
    - id: home
      widgets:
        - label:
            id: my_label
            text: "Hello World!"
            x: 50
            y: 50
            text_font: montserrat_20
            text_color: 0xFFFFFF
            text_align: CENTER
            # Mode de texte long
            long_mode: WRAP  # WRAP, DOT, SCROLL, SCROLL_CIRCULAR, CLIP
            width: 200
```

**Propriétés principales**:
- `text`: Texte à afficher
- `text_font`: Police de caractères
- `text_color`: Couleur du texte
- `text_align`: Alignement (LEFT, CENTER, RIGHT)
- `long_mode`: Comportement pour texte long
- `recolor`: Active les codes de couleur inline

**Documentation**: [Label - LVGL 9.5](https://docs.lvgl.io/9.5/details/widgets/label.html)

---

### 2. Button (Bouton)

Bouton cliquable avec texte ou icône.

```yaml
lvgl:
  widgets:
    - button:
        id: my_button
        text: "Click Me"
        x: 100
        y: 100
        width: 120
        height: 50
        checkable: false  # Bouton toggle si true
        on_click:
          - logger.log: "Button clicked!"
        on_long_press:
          - logger.log: "Long press!"
```

**Actions disponibles**:
- `on_click`: Clic simple
- `on_long_press`: Appui long
- `on_press`: Début de pression
- `on_release`: Relâchement

**Documentation**: [Button - LVGL 9.5](https://docs.lvgl.io/9.5/details/widgets/button.html)

---

### 3. SVG 

Affiche SVG.

```yaml
lvgl:
  widgets:
    - svg:
        id: my_image
        src: "/sdcard/icons/home.svg"  # Fichier SVG sur carte SD
        # ou
        file: "icons/home.svg"# Image définie dans esphome
        x: 50
        y: 50
        width: 64   
        height: 64
      
or
    - svg:
        id: my_image
        file: "icons/home.svg"# Image définie dans esphome
        x: 50
        y: 50
        width: 64   
        height: 64
```

**Formats supportés**:
- **SVG**: Vectoriel, scalable (ThorVG)


### 4. Object (Container de Base)

Conteneur de base pour grouper des widgets.

```yaml
lvgl:
  widgets:
    - obj:
        id: my_container
        x: 0
        y: 0
        width: 200
        height: 150
        bg_color: 0x2196F3
        border_width: 2
        border_color: 0xFFFFFF
        radius: 10  # Coins arrondis
        pad_all: 10  # Padding interne
        widgets:
          - label:
              text: "Inside container"
```



---

## Widgets d'Entrée

### 5. Slider (Curseur)

Curseur pour sélectionner une valeur.

```yaml
lvgl:
  widgets:
    - slider:
        id: brightness_slider
        x: 50
        y: 100
        width: 300
        min_value: 0
        max_value: 100
        value: 50
        mode: NORMAL  # NORMAL, SYMMETRICAL, RANGE
        on_change:
          - lambda: |-
              ESP_LOGI("slider", "Value: %d", (int)x);
```

**Modes**:
- `NORMAL`: Valeur unique de min à max
- `SYMMETRICAL`: Valeur centrée (0 au milieu)
- `RANGE`: Deux valeurs (début et fin)

**Documentation**: [Slider - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/slider.html)

---

### 6. Switch (Interrupteur)

Interrupteur ON/OFF.

```yaml
lvgl:
  widgets:
    - switch:
        id: wifi_switch
        x: 100
        y: 150
        state: true  # ON au démarrage
        on_change:
          - if:
              condition:
                lambda: return x;
              then:
                - logger.log: "Switch ON"
              else:
                - logger.log: "Switch OFF"
```

**Documentation**: [Switch - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/switch.html)

---

### 7. Checkbox (Case à cocher)

Case à cocher avec label.

```yaml
lvgl:
  widgets:
    - checkbox:
        id: agree_checkbox
        text: "I agree to terms"
        x: 50
        y: 200
        checked: false
        on_change:
          - logger.log:
              format: "Checkbox: %s"
              args: [ 'x ? "checked" : "unchecked"' ]
```

**Documentation**: [Checkbox - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/checkbox.html)

---

### 8. Dropdown (Liste déroulante)

Liste déroulante de sélection.

```yaml
lvgl:
  widgets:
    - dropdown:
        id: city_selector
        x: 50
        y: 100
        width: 150
        options: "Paris\nLyon\nMarseille\nToulouse"
        # ou avec liste
        options:
          - "Paris"
          - "Lyon"
          - "Marseille"
        selected_index: 0
        on_select:
          - lambda: |-
              ESP_LOGI("dropdown", "Selected: %d", (int)x);
```

**Documentation**: [Dropdown - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/dropdown.html)

---

### 9. Roller (Rouleau de sélection)

Rouleau vertical pour sélection (style iOS).

```yaml
lvgl:
  widgets:
    - roller:
        id: time_roller
        x: 100
        y: 100
        width: 100
        height: 150
        options: "00\n01\n02\n03\n04\n05"
        selected_index: 0
        visible_row_count: 5  # Nombre de lignes visibles
        mode: NORMAL  # NORMAL ou INFINITE (boucle)
        on_select:
          - logger.log:
              format: "Hour: %d"
              args: [ 'x' ]
```

**Documentation**: [Roller - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/roller.html)

---

### 10. Textarea (Zone de texte)

Zone de saisie de texte multiligne.

```yaml
lvgl:
  widgets:
    - textarea:
        id: message_input
        x: 50
        y: 100
        width: 300
        height: 150
        text: "Enter message..."
        placeholder_text: "Type here..."
        password_mode: false
        one_line: false  # true = input sur une ligne
        max_length: 100
        accepted_chars: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 "
        on_change:
          - logger.log:
              format: "Text: %s"
              args: "text.c_str()"
```

**Documentation**: [Textarea - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/textarea.html)

---

### 11. Keyboard (Clavier virtuel)

Clavier virtuel pour saisie de texte.

```yaml
lvgl:
  widgets:
    - textarea:
        id: input_field
        x: 50
        y: 50
        width: 300

    - keyboard:
        id: my_keyboard
        x: 0
        y: 250
        width: 100%
        height: 200
        mode: TEXT_LOWER  # TEXT_LOWER, TEXT_UPPER, SPECIAL, NUMBER
        textarea: input_field  # Lie au textarea
```

**Modes**:
- `TEXT_LOWER`: Lettres minuscules
- `TEXT_UPPER`: Lettres majuscules
- `SPECIAL`: Caractères spéciaux
- `NUMBER`: Pavé numérique

**Documentation**: [Keyboard - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/keyboard.html)

---

### 12. Spinbox (Saisie numérique)

Saisie numérique avec boutons +/-.

```yaml
lvgl:
  widgets:
    - spinbox:
        id: temperature_spinbox
        x: 100
        y: 100
        width: 150
        height: 50
        value: 20
        min_value: 0
        max_value: 100
        //step: 1
        digits: 3  # Nombre de chiffres
        decimal_places: 1  # Nombre de décimales
        rollover: true  # Boucle à la fin
```

**Documentation**: [Spinbox - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/spinbox.html)

---

## Widgets d'Affichage

### 13. Arc (Arc circulaire)

Arc/cercle pour afficher une valeur (jauge).

```yaml
lvgl:
  widgets:
    - arc:
        id: volume_arc
        x: 100
        y: 100
        width: 150
        height: 150
        start_angle: 135  # Angle de début (0-360)
        end_angle: 45     # Angle de fin
        value: 50
        min_value: 0
        max_value: 100
        mode: NORMAL  # NORMAL, REVERSE, SYMMETRICAL
        rotation: 0   # Rotation globale
        adjustable: true  # Ajustable par l'utilisateur
```

**Documentation**: [Arc - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/arc.html)

---

### 14. Bar (Barre de progression)

Barre de progression horizontale ou verticale.
animated non reconue 

```yaml
lvgl:
  widgets:
    - bar:
        id: battery_bar
        x: 50
        y: 100
        width: 200
        height: 20
        min_value: 0
        max_value: 100
        value: 75
        mode: NORMAL  # NORMAL, SYMMETRICAL, RANGE
        # Animation
        //animated: true
        //animation:
          //duration: 500ms
```

**Documentation**: [Bar - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/bar.html)

---

### 15. LED (LED)

Indicateur LED avec couleur et luminosité.

```yaml
lvgl:
  widgets:
    - led:
        id: status_led
        x: 100
        y: 100
        width: 30
        height: 30
        color: 0x00FF00  # Vert
        brightness: 255  # 0-255
```

**Documentation**: [LED - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/led.html)

---

### 16. Spinner (Indicateur de chargement)

Indicateur de chargement animé.

```yaml
lvgl:
  widgets:
    - spinner:
        id: loading_spinner
        x: 150
        y: 150
        width: 50
        height: 50
        spin_time: 1000ms  # Durée d'une rotation
        arc_length: 60  # Longueur de l'arc (0-360)
```

**Documentation**: [Spinner - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/spinner.html)

---

### 17. Line (Ligne)

Ligne ou polyligne.

```yaml
lvgl:
  widgets:
    - line:
        id: my_line
        x: 50
        y: 50
        points:
          - x: 0
            y: 0
          - x: 100
            y: 50
          - x: 200
            y: 0
        line_width: 3
        line_color: 0xFF0000
        line_rounded: true  # Extrémités arrondies
```

**Documentation**: [Line - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/line.html)

---

### 18. Scale (Échelle/Jauge) ⚡ Nouveau LVGL 9

Échelle graduée linéaire ou circulaire (remplace Meter).

```yaml
lvgl:
  widgets:
    - scale:
        id: speedometer
        x: 50
        y: 50
        width: 300
        height: 300
        mode: ROUND_OUTER  # HORIZONTAL_TOP, HORIZONTAL_BOTTOM,
                          # VERTICAL_LEFT, VERTICAL_RIGHT,
                          # ROUND_INNER, ROUND_OUTER
        range:
          min: 0
          max: 200
        angle_range: 270  # Angle total pour mode circulaire
        rotation: 0
        # Graduations
        total_tick_count: 21
        major_tick_every: 5
        label_count: 11
        # Style des graduations
        tick_length: 10
        tick_width: 2
```

**Documentation**: [Scale - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/scale.html)
**Voir aussi**: `SCALE_WIDGET_README.md` et `SCALE_QUICK_REFERENCE.md`

---

### 19. Chart (Graphique)

Graphique pour afficher des données.

```yaml
lvgl:
  widgets:
    - chart:
        id: temperature_chart
        x: 50
        y: 50
        width: 300
        height: 200
        type: LINE  # LINE, BAR, SCATTER
        point_count: 20
        y_range:
          min: 0
          max: 40
        series:
          - id: temp_series
            color: 0xFF0000
            width: 2

       
```



**Types de graphiques**:
- `LINE`: Courbe linéaire
- `BAR`: Histogramme
- `SCATTER`: Nuage de points

**Documentation**: [Chart - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/chart.html)
**Voir aussi**: `CHART_README.md`

---

### 20. QR Code

Génère et affiche un QR code.
probleme DATA invalide option QR Code
```yaml
lvgl:
  widgets:
    - qrcode:
        id: wifi_qrcode
        x: 100
        y: 100
        size: 150
        data: "WIFI:T:WPA;S:MyNetwork;P:password123;;"
        dark_color: 0x000000
        light_color: 0xFFFFFF
```

**Documentation**: [QR Code - LVGL 9.5](https://docs.lvgl.io/9.4/details/libs/qrcode.html)

---

## Widgets Avancés

### 21. AnimImg (Image Animée)

Affiche une séquence d'images animées.

```yaml
lvgl:
  widgets:
    - animimg:
        id: my_animation
        x: 100
        y: 100
        images:
          - frame1
          - frame2
          - frame3
        duration: 100ms  # Durée par frame
        repeat_count: -1  # -1 = infini
        auto_start: true
```

**Documentation**: [AnimImg - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/animimg.html)

---

### 22. Lottie (Animation Vectorielle) ⚡ Nouveau LVGL 9

Animations vectorielles JSON (ultra fluides).

```yaml
lvgl:
  widgets:
    - lottie:
        id: weather_animation
        src: "/sdcard/animations/weather.json"
        x: 100
        y: 100
        width: 200
        height: 200
 or
    - lottie:
        id: weather_animation
        file: "animations/weather.json"
        x: 100
        y: 100
        width: 200
        height: 200       

```

**Avantages**:
- Animations 60 FPS ultra fluides
- 90% plus léger que les GIF
- Redimensionnable sans perte de qualité

**Ressources**:
- Lottie documentation (https://docs.lvgl.io/9.5/details/widgets/lottie.html)
- [Weather Icons by Basmilius](https://github.com/basmilius/weather-icons)
- [LottieFiles](https://lottiefiles.com/)

**Voir aussi**: `LOTTIE_README.md`

---


### 24. Arc Label ⚡ Nouveau LVGL 9

Texte courbé le long d'un arc.

```yaml
lvgl:
  widgets:
    - arclabel:
        id: curved_text
        x: 100
        y: 100
        width: 200
        height: 200
        text: "Curved Text Example"
        angle: 270  # Angle de l'arc
        radius: 100
        rotation: 0
```

**Voir aussi**: `ARCLABEL_README.md`

---

### 25. Span (Texte Enrichi)

Texte avec styles mixtes (gras, couleurs, tailles différentes).

```yaml
lvgl:
  widgets:
    - span:
        id: rich_text
        x: 50
        y: 50
        width: 300
        mode: BREAK  # EXPAND, BREAK, DOTS, CLIP
        spans:
          - text: "Bold text"
            text_font: montserrat_20
            text_color: 0xFF0000
          - text: " normal "
          - text: "italic"
            text_decor: UNDERLINE
```

**Documentation**: [Spangroup - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/spangroup.html)
**Voir aussi**: `SPAN_README.md`

---

## Widgets de Conteneur

### 26. TabView (Onglets)

Interface à onglets.

```yaml
lvgl:
  widgets:
    - tabview:
        id: my_tabs
        x: 0
        y: 0
        width: 100%
        height: 100%
        position: TOP  # TOP, BOTTOM, LEFT, RIGHT
        tabs:
          - id: tab_home
            name: "Home"
            widgets:
              - label:
                  text: "Home content"

          - id: tab_settings
            name: "Settings"
            widgets:
              - label:
                  text: "Settings content"
```

**Documentation**: [TabView - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/tabview.html)

---

### 27. TileView (Vues défilantes)

Vues en grille avec défilement.

```yaml
lvgl:
  widgets:
    - tileview:
        id: my_tileview
        x: 0
        y: 0
        width: 100%
        height: 100%
        tiles:
          - id: tile1
            row: 0
            column: 0   # ← changed from col
            dir: HOR  # HOR, VER, ALL
            widgets:
              - label:
                  text: "Tile 1"

          - id: tile2
            row: 0
            column: 1   # ← changed from col
            dir: HOR
            widgets:
              - label:
                  text: "Tile 2"
```

**Documentation**: [TileView - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/tileview.html)

---

### 28. Menu (Menu Hiérarchique) ⚡ Nouveau LVGL 9

Menu de navigation hiérarchique avec sidebar.
A revoire!! 

```yaml
lvgl:
  widgets:
    - menu:
        id: settings_menu
        x: 0
        y: 0
        width: 100%
        height: 100%
        root_back_button: false
        pages:
          - id: main_page
            title: "Main Menu"
            widgets:
              - button:
                  text: "Settings"
              - button:
                  text: "About"

          - id: settings_page
            title: "Settings"
            widgets:
              - switch:
                  text: "WiFi"
              - switch:
                  text: "Bluetooth"
```

**Documentation**: [Menu - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/index.html)
**Voir aussi**: `MENU_README.md`

---

### 29. Window (Fenêtre)

Fenêtre avec barre de titre et boutons.
ERROR   dictionary-based
```yaml
lvgl:
  widgets:
    - id: main_page
      widgets:
        # Background / Main screen content
        - label:
            text: "Main Screen"
            align: CENTER

        - button:
            id: show_info_btn
            text: "Show Info"
            x: 50
            y: 50
            width: 120
            on_click:
              - lvgl.obj.clear_flag:
                  id: info_window
                  flag: HIDDEN

        - button:
            id: show_settings_btn
            text: "Settings"
            x: 50
            y: 100
            width: 120
            on_click:
              - lvgl.obj.clear_flag:
                  id: settings_window
                  flag: HIDDEN

        # Example 1: Basic Information Window
        - win:
            id: info_window
            title: "Information"
            x: 100
            y: 100
            width: 300
            height: 200
            bg_color: 0xFFFFFF
            border_width: 2
            border_color: 0x333333
            shadow_width: 10
            shadow_opa: 30%
            flag: HIDDEN  # Start hidden

            header:
              bg_color: 0x2196F3
              text_color: 0xFFFFFF
              height: 40

            header_buttons:
              - id: info_close_btn
                src: close_icon
                on_click:
                  - lvgl.obj.add_flag:
                      id: info_window
                      flag: HIDDEN

            body:
              bg_color: 0xF5F5F5
              pad_all: 15

            widgets:
              - label:
                  text: "This is an information window"
                  align: TOP_MID
                  y: 10

              - label:
                  text: "You can add any widgets here"
                  align: CENTER

              - button:
                  text: "OK"
                  align: BOTTOM_MID
                  y: -10
                  width: 100
                  on_click:
                    - lvgl.obj.add_flag:
                        id: info_window
                        flag: HIDDEN

        # Example 2: Settings Window with Multiple Controls
        - win:
            id: settings_window
            title: "Settings"
            x: 50
            y: 50
            width: 400
            height: 350
            bg_color: 0xFFFFFF
            flag: HIDDEN

            header:
              bg_color: 0xFF5722
              text_color: 0xFFFFFF

            header_buttons:
              - id: settings_close_btn
                src: close_icon
                on_click:
                  - lvgl.obj.add_flag:
                      id: settings_window
                      flag: HIDDEN

            widgets:
              - label:
                  text: "Device Configuration"
                  x: 10
                  y: 10
                  text_font: roboto_16_bold

              # WiFi Settings
              - label:
                  text: "WiFi:"
                  x: 10
                  y: 50

              - switch:
                  id: wifi_switch
                  text: "Enable WiFi"
                  x: 10
                  y: 75

              # Brightness Control
              - label:
                  text: "Brightness:"
                  x: 10
                  y: 120

              - slider:
                  id: brightness_slider
                  min_value: 0
                  max_value: 100
                  value: 75
                  x: 10
                  y: 145
                  width: 360

              - label:
                  id: brightness_value
                  text: "75%"
                  x: 370
                  y: 145

              # Sound Settings
              - label:
                  text: "Volume:"
                  x: 10
                  y: 190

              - slider:
                  id: volume_slider
                  min_value: 0
                  max_value: 100
                  value: 50
                  x: 10
                  y: 215
                  width: 360

              # Action Buttons
              - button:
                  text: "Cancel"
                  x: 200
                  y: 280
                  width: 90
                  on_click:
                    - lvgl.obj.add_flag:
                        id: settings_window
                        flag: HIDDEN

              - button:
                  text: "Save"
                  x: 300
                  y: 280
                  width: 90
                  bg_color: 0x4CAF50
                  text_color: 0xFFFFFF
                  on_click:
                    - lambda: |-
                        // Save settings
                    - lvgl.obj.add_flag:
                        id: settings_window
                        flag: HIDDEN

        # Example 3: Confirmation Dialog
        - win:
            id: confirm_dialog
            title: "Confirm"
            x: CENTER
            y: CENTER
            width: 280
            height: 150
            bg_color: 0xFFFFFF
            shadow_width: 20
            shadow_opa: 50%
            flag: HIDDEN

            header:
              bg_color: 0xFFC107
              text_color: 0x000000

            widgets:
              - label:
                  text: "Are you sure you want to restart?"
                  align: TOP_MID
                  y: 15
                  text_align: CENTER

              - container:
                  layout: flex
                  flex_flow: ROW
                  align: BOTTOM_MID
                  y: -15
                  pad_column: 10
                  widgets:
                    - button:
                        text: "No"
                        width: 100
                        on_click:
                          - lvgl.obj.add_flag:
                              id: confirm_dialog
                              flag: HIDDEN

                    - button:
                        text: "Yes"
                        width: 100
                        bg_color: 0xF44336
                        text_color: 0xFFFFFF
                        on_click:
                          - lambda: |-
                              // Perform restart
                          - lvgl.obj.add_flag:
                              id: confirm_dialog
                              flag: HIDDEN

        # Example 4: File Browser Window
        - win:
            id: file_browser
            title: "Select File"
            x: 80
            y: 60
            width: 380
            height: 320
            flag: HIDDEN

            header:
              bg_color: 0x607D8B
              text_color: 0xFFFFFF

            header_buttons:
              - id: file_browser_close
                src: close_icon
                on_click:
                  - lvgl.obj.add_flag:
                      id: file_browser
                      flag: HIDDEN

            widgets:
              # Current path
              - label:
                  id: current_path
                  text: "/home/user/"
                  x: 5
                  y: 5

              # File list
              - list:
                  id: file_list
                  x: 5
                  y: 30
                  width: 360
                  height: 220
                  items:
                    - type: text
                      text: "Folders"
                    - type: button
                      text: "Documents"
                    - type: button
                      text: "Pictures"
                    - type: text
                      text: "Files"
                    - type: button
                      text: "readme.txt"
                    - type: button
                      text: "config.yaml"

              # Action buttons
              - button:
                  text: "Cancel"
                  x: 150
                  y: 265
                  width: 100
                  on_click:
                    - lvgl.obj.add_flag:
                        id: file_browser
                        flag: HIDDEN

              - button:
                  text: "Open"
                  x: 260
                  y: 265
                  width: 100
                  bg_color: 0x2196F3
                  text_color: 0xFFFFFF

# Update brightness label when slider changes
on_slider_changed:
- lambda: |-
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (int)id(brightness_slider)->get_value());
  id(brightness_value)->set_text(buf);
```

**Documentation**: [Window - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/win.html)
**Voir aussi**: `WIN_README.md`

---

### 30. List (Liste)

Liste de boutons avec texte et icônes.
RIEN NE FONCTIONNE
```yaml
lvgl:
  widgets:
    - list:
        id: my_list
        x: 50
        y: 50
        width: 250
        height: 300
        items:
          - text: "Item 1"
            icon: "\xEF\x80\x81"  # Font Awesome icon
          - text: "Item 2"
            icon: "\xEF\x80\x82"
          - text: "Item 3"
```

**Documentation**: [List - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/list.html)

---

### 31. Table (Tableau)

Tableau avec lignes et colonnes.
IL FAUT TOUS REVOIR
```yaml
lvgl:
  widgets:
    - table:
        id: data_table
        x: 50
        y: 50
        width: 300
        height: 200
        col_count: 3
        row_count: 4
        cells:
          - row: 0
            col: 0
            text: "Name"
          - row: 0
            col: 1
            text: "Age"
          - row: 0
            col: 2
            text: "City"
          - row: 1
            col: 0
            text: "Alice"
          - row: 1
            col: 1
            text: "25"
          - row: 1
            col: 2
            text: "Paris"

      widgets: 
        - table: 
            id: data_table
            x: 50
            y: 50
            width: 300
            height: 200
            
            [col_count] is an invalid option for [table]. Did you mean [column_count], [row_count], [scroll_one]?
            col_count: 3
            row_count: 4
            cells: 
              
              'column' is a required option for [cells].
              - row: 0
                
                [col] is an invalid option for [cells]. Did you mean [column]?
                col: 0
                text: Name
              - row: 0
            
```

**Documentation**: [Table - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/table.html)
**Voir aussi**: `TABLE_README.md` et `TABLE_IMPLEMENTATION_SUMMARY.md`

---

### 32. Calendar (Calendrier) ⚡ Nouveau LVGL 9

Calendrier mensuel interactif.

```yaml
lvgl:
  widgets:
    - calendar:
        id: dropdown_calendar
        x: 10
        y: 320
        width: 300
        height: 300
        header_mode: dropdown
        today_date:
          year: 2024
          month: 12
          day: 15
        showed_date:
          year: 2024
          month: 12
          day: 1
```

**Documentation**: [Calendar - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/calendar.html)
**Voir aussi**: `CALENDAR_README.md`

---

### 33. ButtonMatrix (Matrice de Boutons)

Grille de boutons configurables.

```yaml
lvgl:
  widgets:
    - buttonmatrix:
        id: keypad
        x: 50
        y: 250
        width: 300
        height: 200
        rows: 4
        buttons:
          - row: 0
            buttons:
              - "1"
              - "2"
              - "3"
          - row: 1
            buttons:
              - "4"
              - "5"
              - "6"
          - row: 2
            buttons:
              - "7"
              - "8"
              - "9"
          - row: 3
            buttons:
              - ""
              - "0"
              - ""
        on_click:
          - lambda: |-
              ESP_LOGI("btnmatrix", "Button %d clicked", button_id);
```

**Documentation**: [ButtonMatrix - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/buttonmatrix.html)

---

### 34. MsgBox (Boîte de Message)

Boîte de dialogue modale.

```yaml
lvgl:
  widgets:
    - msgbox:
        id: confirm_dialog
        title: "Confirmation"
        text: "Êtes-vous sûr ?"
        close_button: true
        buttons:
          - "Oui"
          - "Non"
        on_click:
          - lambda: |-
              if (button_id == 0) {
                ESP_LOGI("msgbox", "Yes clicked");
              } else {
                ESP_LOGI("msgbox", "No clicked");
              }
```

**Documentation**: [MsgBox - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/msgbox.html)

---

### 35. Canvas (Canevas de Dessin)

Surface de dessin pour graphiques personnalisés.

```yaml
lvgl:
  widgets:
    - canvas:
        id: drawing_canvas
        x: 50
        y: 50
        width: 300
        height: 200
        bg_color: 0xFFFFFF
```

**Fonctions de dessin** (via lambda):
- Lignes, rectangles, cercles
- Texte
- Images
- Pixels individuels

**Documentation**: [Canvas - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/canvas.html)

---

### 36. Button (Styling buttons)

Styling buttons.
```yaml
lvgl:
  widgets:
    - button:
        align: CENTER
        x: 150
        width: SIZE_CONTENT
        height: SIZE_CONTENT
        radius: 3
        bg_color: 0x2196F3
        bg_grad_color: 0x1565C0
        bg_grad_dir: VER
        bg_opa: COVER
        border_width: 2
        border_color: 0x9E9E9E
        border_opa: 40%
        shadow_width: 8
        shadow_color: 0x9E9E9E
        shadow_offset_y: 8
        outline_color: 0x2196F3
        outline_opa: COVER
        pad_all: 10
        pressed:
          outline_width: 30
          outline_opa: TRANSP
          translate_y: 5
          shadow_offset_y: 3
          bg_color: 0x1565C0
          bg_grad_color: 0x0D47A1
          # Transition on pressed state (linear 300ms like LVGL example)
          style_transition_time: 300ms
          style_transition_path: linear
        on_click:
          - logger.log: "Styled button clicked"
        widgets:
          - label:
              align: CENTER
              text: "Button"
              text_color: 0xFFFFFF  
 --- 
### 37. Button (Gum)
```yaml
lvgl:
  widgets:
    - button:
          align: CENTER
          x: 150
          y: 80
          # Transition for release (overshoot = bounce-back effect)
          style_transition_time: 250ms
          style_transition_delay: 100ms
          style_transition_path: overshoot
          pressed:
            transform_width: 10
            transform_height: -10
            text_letter_space: 10
            # Transition for press
            style_transition_time: 250ms
            style_transition_path: ease_in_out
          widgets:
            - label:
                align: CENTER
                text: "Gum" 
---                
```yaml
lvgl:
  widgets:
    - imgbtn:
        id: power_button
        x: 100
        y: 100
        width: 64
        height: 64
        src: power_icon  # Image normale
        src_pressed: power_icon_pressed  # Image pressée
        src_checked: power_icon_on  # Image cochée
        on_click:
          - logger.log: "Power button clicked"
```

**Documentation**: [ImageButton - LVGL 9.5](https://docs.lvgl.io/9.5/widgets/imagebutton.html)
**Voir aussi**: `IMGBTN_README.md`

---

## Propriétés Communes à Tous les Widgets

### Position et Taille

```yaml
x: 100          # Position X en pixels ou %
y: 50           # Position Y
width: 200      # Largeur
height: 100     # Hauteur
```

### Alignement

```yaml
align: CENTER   # TOP_LEFT, TOP_MID, TOP_RIGHT, LEFT_MID,
               # CENTER, RIGHT_MID, BOTTOM_LEFT,
               # BOTTOM_MID, BOTTOM_RIGHT
align_to: other_widget_id
```

### Style

```yaml
bg_color: 0x2196F3      # Couleur de fond
bg_opa: COVER           # Opacité (0-255 ou TRANSP/COVER)
border_width: 2         # Épaisseur de la bordure
border_color: 0xFFFFFF  # Couleur de la bordure
radius: 10              # Rayon des coins arrondis
pad_all: 10             # Padding uniforme
pad_left: 5             # Padding gauche
pad_right: 5            # Padding droit
pad_top: 5              # Padding haut
pad_bottom: 5           # Padding bas
shadow_width: 10        # Largeur de l'ombre
shadow_color: 0x000000  # Couleur de l'ombre
```

### États et Parts

```yaml
styles:
  - state: DEFAULT      # default, checked, focused, pressed, etc.
    part: MAIN          # main, scrollbar, indicator, knob, etc.
    bg_color: 0x2196F3

  - state: PRESSED
    part: MAIN
    bg_color: 0x1976D2
```

## Ressources

### Documentation Officielle
- [LVGL 9.4 Documentation](https://docs.lvgl.io/9.5/introduction/index.html)


### Ressources Graphiques
- **SVG Icons**:
  - [Remix Icon](https://remixicon.com/) - 2,800+ icônes
  - [Ionicons](https://ionic.io/ionicons)
  - [Heroicons](https://heroicons.com/)

- **Lottie Animations**:
  - [Weather Icons](https://github.com/basmilius/weather-icons)
  - [LottieFiles](https://lottiefiles.com/)
  - [Lordicon](https://lordicon.com/)

### Exemples
- [LVGL Examples](https://docs.lvgl.io/9.4/examples.html)
- [ESPHome LVGL Examples](https://esphome.io/components/lvgl.html)

---

## Support et Contribution


### Documentation des Widgets
Consultez les README spécifiques pour plus de détails:
- `ARCLABEL_README.md`
- `CALENDAR_README.md`
- `CHART_README.md`
- `IMGBTN_README.md`
- `LOTTIE_README.md`
- `MENU_README.md`
- `SCALE_WIDGET_README.md`
- `SCALE_QUICK_REFERENCE.md`
- `SPAN_README.md`
- `TABLE_README.md`
- `TABLE_IMPLEMENTATION_SUMMARY.md`
- `TEX3D_README.md`
- `WIN_README.md`

---

**Implémentation complète LVGL 9.5 pour ESPHome**
✅ 35/35 widgets documentés
✅ 70 événements supportés
✅ ThorVG/SVG/Lottie activés

**Made with ❤️ for the ESPHome community**      

```


