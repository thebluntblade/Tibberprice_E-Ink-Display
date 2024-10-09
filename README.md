The project is largely based on the source code from https://github.com/MakeMagazinDE/Preisrahmen, but uses the T5 4.7" display from Lilygo (https://www.lilygo.cc/products/t5-4-7-inch-e-paper-v2-3), i.e. a combination of ESP32 and EPD (e-paper display). The instructions for adapting the source code to the Lilygo can be found here: https://github.com/Xinyuan-LilyGO/LilyGo-EPD47.

The case I used is from Thingiverse https://www.thingiverse.com/thing:6167178

Compared to the original device based on the link above, this solution has some advantages and disadvantages.

Advantages:
- Easier, as you just buy the combined EPD with ESP.
- Cheaper for the same reason.
  
Disadvantages:
- Smaller display.
- No battery and probably much higher power consumption as there is no hardware optimisation.

How to:
- Rename Credentials-example.h to Credentials.h and add your SSID, password and tibber api token.
