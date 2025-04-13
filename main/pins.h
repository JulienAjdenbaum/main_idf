#ifndef PINS_H
#define PINS_H

// // I2S pins, etc.
// #define I2S_NUM         (I2S_NUM_0)
// #define I2S_BCK_IO      (26)
// #define I2S_WS_IO       (25)
// #define I2S_DO_IO       (27)
// #define I2S_DI_IO       (-1) // not used

// #define MIC_I2S_PORT            I2S_NUM_1
// #define MIC_I2S_BCK_IO          14
// #define MIC_I2S_WS_IO           13
// #define MIC_I2S_DATA_IN_IO      12
// #define MIC_I2S_DATA_OUT_IO     -1 
// #define MIC_USE_APLL            false

// #define BUTTON_GPIO                  4                  // GPIO for push button
// #define RMT_LED_STRIP_GPIO_NUM       15
// #define EXAMPLE_LED_NUMBERS          4                  // Number of LEDs in the strip

// #define RC522_SPI_BUS_GPIO_MISO    (34)
// #define RC522_SPI_BUS_GPIO_MOSI    (23)
// #define RC522_SPI_BUS_GPIO_SCLK    (32)
// #define RC522_SPI_SCANNER_GPIO_SDA (33)
// #define RC522_SCANNER_GPIO_RST     (-1) // soft-reset

// #define POT_PIN ADC1_CHANNEL_7  // Adjust as needed

// I2S pins, etc.
#define I2S_NUM         (I2S_NUM_0)

#define I2S_BCK_IO      (25)
#define I2S_WS_IO       (27)
#define I2S_DO_IO       (26)
#define I2S_DI_IO       (-1) // not used

#define MIC_I2S_PORT            I2S_NUM_1
#define MIC_I2S_BCK_IO          15
#define MIC_I2S_WS_IO           4
#define MIC_I2S_DATA_IN_IO      2
#define MIC_I2S_DATA_OUT_IO     -1 
#define MIC_USE_APLL            false

#define BUTTON_GPIO                  14                  // GPIO for push button
#define RMT_LED_STRIP_GPIO_NUM       13

#define RC522_SPI_BUS_GPIO_MISO    (36)
#define RC522_SPI_BUS_GPIO_MOSI    (40)
#define RC522_SPI_BUS_GPIO_SCLK    (41)
#define RC522_SPI_SCANNER_GPIO_SDA (42)
#define RC522_SCANNER_GPIO_RST     (-1) // soft-reset

#define POT_PIN ADC1_CHANNEL_6  // Adjust as needed
#define EXAMPLE_LED_NUMBERS          4                  // Number of LEDs in the strip

#endif // PINS_H