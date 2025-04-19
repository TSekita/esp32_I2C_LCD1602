#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "myheader.h"
#include "dht.h" 

#define DHT_GPIO GPIO_NUM_4

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

#define I2C_MASTER_NUM           I2C_NUM_0
#define I2C_MASTER_SCL_IO        22
#define I2C_MASTER_SDA_IO        21
#define I2C_MASTER_FREQ_HZ       100000
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0
#define I2C_MASTER_TIMEOUT_MS    1000

#define I2C_MASTER_WRITE 0
#define I2C_MASTER_READ  1

#define LCD_ADDR 0x27  // Change as needed

#define LCD_BACKLIGHT   0x08
#define ENABLE          0x04
#define RW              0x02
#define RS              0x01

uint8_t degree_char[8] = {
    0b01000,
    0b10100,
    0b01000,
    0b00111,
    0b01000,
    0b01000,
    0b00111,
    0b00000
};

uint8_t humidity_char[8] = {
    0b00100,
    0b00100,
    0b01110,
    0b11111,
    0b11111,
    0b11111,
    0b01110,
    0b00000
};

void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode,
                       I2C_MASTER_RX_BUF_DISABLE,
                       I2C_MASTER_TX_BUF_DISABLE, 0);
}

void lcd_send_cmd(uint8_t cmd)
{
    uint8_t upper = cmd & 0xF0;
    uint8_t lower = (cmd << 4) & 0xF0;
    uint8_t data_t[4];

    data_t[0] = upper | LCD_BACKLIGHT | ENABLE;
    data_t[1] = upper | LCD_BACKLIGHT;
    data_t[2] = lower | LCD_BACKLIGHT | ENABLE;
    data_t[3] = lower | LCD_BACKLIGHT;

    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (LCD_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(handle, data_t, sizeof(data_t), true);
    i2c_master_stop(handle);
    i2c_master_cmd_begin(I2C_MASTER_NUM, handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(handle);

    vTaskDelay(pdMS_TO_TICKS(5));
}

void lcd_send_data(uint8_t data)
{
    uint8_t upper = data & 0xF0;
    uint8_t lower = (data << 4) & 0xF0;
    uint8_t data_t[4];

    data_t[0] = upper | LCD_BACKLIGHT | ENABLE | RS;
    data_t[1] = upper | LCD_BACKLIGHT | RS;
    data_t[2] = lower | LCD_BACKLIGHT | ENABLE | RS;
    data_t[3] = lower | LCD_BACKLIGHT | RS;

    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (LCD_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(handle, data_t, sizeof(data_t), true);
    i2c_master_stop(handle);
    i2c_master_cmd_begin(I2C_MASTER_NUM, handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(handle);

    vTaskDelay(pdMS_TO_TICKS(5));
}

void lcd_send_string(const char *str)
{
    while (*str)
    {
        lcd_send_data((uint8_t)(*str));
        str++;
    }
}

void lcd_init(void)
{
    lcd_send_cmd(0x33); // Initialization sequence
    lcd_send_cmd(0x32); // Continue initialization
    lcd_send_cmd(0x28); // 2-line display mode
    lcd_send_cmd(0x0C); // Display ON, cursor OFF
    lcd_send_cmd(0x06); // Entry mode set
    lcd_send_cmd(0x01); // Clear display
    vTaskDelay(pdMS_TO_TICKS(10));
}

void initialize_time()
{
    // Get the current time from an NTP server (Wi-Fi connection required)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait until the time is successfully obtained
    time_t now = 0;
    struct tm timeinfo = {0};
    while (timeinfo.tm_year < (2020 - 1900)) {
        time(&now);
        localtime_r(&now, &timeinfo);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void lcd_create_custom_char(uint8_t location, uint8_t charmap[])
{
    // location: 0 to 7 (CGRAM slot number)
    location &= 0x07;

    // Send CGRAM address set command
    lcd_send_cmd(0x40 | (location << 3));

    // Write the character pattern in 8 bytes
    for (int i = 0; i < 8; i++) {
        lcd_send_data(charmap[i]);
    }
}

void display_time_on_lcd()
{
    time_t now;
    struct tm timeinfo;
    char line1[32];
    char line2[17];
    int16_t temperature = 0, humidity = 0;
    const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        // Retrieve both temperature and humidity from DHT11 in one call
        esp_err_t res = dht_read_data(DHT_TYPE_DHT11, DHT_GPIO, &humidity, &temperature);
        float temp = (res == ESP_OK) ? (float)temperature / 10.0 : -100.0;
        float hum = (res == ESP_OK) ? (float)humidity / 10.0 : -1.0;
        ESP_LOGI("DHT", "Temperature: %.1f°C, Humidity: %.1f%%", temp, hum);
        // Line 1: Date and weekda
        snprintf(line1, sizeof(line1), "%04d%02d%02d %s %2.0f",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 weekdays[timeinfo.tm_wday], temp);

        // Line 2: Time and humidity
        snprintf(line2, sizeof(line2), "%02d:%02d:%02d     %2.0f%%",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 hum);

        lcd_send_cmd(0x80);                 // Set cursor to the beginning of line 1
        lcd_send_string(line1);
        lcd_send_data(0x00);                // Display character from slot 0 (degree Celsius symbol)
        lcd_send_cmd(0xC0);                 // Set cursor to the beginning of line 2
        lcd_send_string(line2);
        lcd_send_cmd(0xC0 + 12);            // Move cursor to position 12 of line 2
        lcd_send_data(0x01);                // Display character from slot 1 (humidity symbol)

        vTaskDelay(pdMS_TO_TICKS(1000));    // Update every 1 second
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect(); // Reconnect to Wi-Fi
        ESP_LOGI("wifi", "retry to connect");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("wifi", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI("wifi", "wifi_init_sta finished.");
}


void app_main(void)
{
    i2c_master_init();
    lcd_init();
    
    // Initialize NVS (required for storing Wi-Fi settings) 
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    wifi_init_sta();

    // Wait until Wi-Fi connection is established
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    lcd_create_custom_char(0, degree_char);  // Register degree symbol to slot 0
    lcd_create_custom_char(1, humidity_char);  // Register humidity symbol to slot 1

    initialize_time();         // Get the current time from the network
    setenv("TZ", "JST-9", 1);
    tzset();
    display_time_on_lcd();     // Display on the LCD
}