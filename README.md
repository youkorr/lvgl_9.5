
---

### 3. THORVG 

Affiche  LOTTIE ou SVG.

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

    - lottie:
        id: my_image
        file: "icons/home.json"  # Fichier SVG sur carte SD
        # ou
        src: "/sdcard/icons/home.json" # Image définie dans esphome
        x: 50
        y: 50
        width: 64   
        height: 64

  
**Documentation**: [Image - LVGL 9.4](https://docs.lvgl.io/9.4/details/widgets/image.html)        

```



https://github.com/user-attachments/assets/6787cd1f-ee36-4cc4-836f-5805008d07ae




https://github.com/user-attachments/assets/37b924ab-ba2f-4925-a49b-93fccd4cf848



https://github.com/user-attachments/assets/f0c77033-1ffc-4adc-8356-ef7a395cf937

