idf_component_register(SRCS "main.c" "st7789.c" "cats565.c" "gamepad.c" "snake.c" "arkanoid.c" "gamemenu.c" "buzzer.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi)
