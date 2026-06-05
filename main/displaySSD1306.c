#include "driver/i2c_master.h"
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "displaySSD1306.h"

static const char *TAG = "SSD1306";

//I2C Port Configuration for display
#define I2C_DISPLAY_BUS_PORT     0
#define I2C_DISPLAY_SDA          5
#define I2C_DISPLAY_SCL          4

//I2C Bus Handler and Configuration for display
i2c_master_bus_handle_t i2c_display_bus = NULL;    
i2c_master_bus_config_t display_bus_config = 
{
    .i2c_port = I2C_DISPLAY_BUS_PORT,
    .sda_io_num = I2C_DISPLAY_SDA,
    .scl_io_num = I2C_DISPLAY_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,    
    .flags.enable_internal_pullup = true    
};

esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_io_i2c_config_t io_config = 
{
    .dev_addr            = 0x3C,    
    .control_phase_bytes = 1,   // According to SSD1306 datasheet
    .dc_bit_offset       = 6,   // According to SSD1306 datasheet
    .lcd_cmd_bits        = 8,   // According to SSD1306 datasheet    
    .lcd_param_bits      = 8,   // According to SSD1306 datasheet    
    .scl_speed_hz        = 400000,
};

void displayInit()
{
    //------------------------------------------------
    // SSD1306 Initialization
    //------------------------------------------------
    ESP_LOGI(TAG, "Install panel IO");
    ESP_ERROR_CHECK(i2c_new_master_bus(&display_bus_config, &i2c_display_bus));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_display_bus, &io_config, &io_handle));
    

    ESP_LOGI(TAG, "Install SSD1306 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = 
    {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_config = 
    {
        .height = LCD_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    esp_lcd_panel_io_tx_param(io_handle, 0xA7, NULL, 0);  // 0xA7 = invertido, 0xA6 = normal
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    //------------------------------------------------
    
    //------------------------------------------------
    // LVGL Initialization
    //------------------------------------------------
    ESP_LOGI(TAG, "Initialize LVGL");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    const lvgl_port_display_cfg_t disp_cfg = 
    {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = true,
        .rotation = 
        {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        }
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);

    /* Rotation of the screen */
    lv_disp_set_rotation(disp, LV_DISP_ROT_NONE);
    //------------------------------------------------
}