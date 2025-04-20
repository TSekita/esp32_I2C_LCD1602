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
#include "myheader.h"               // #define WIFI_SSID , #define WIFI_PASS
#include "dht.h" 

#define DHT_GPIO GPIO_NUM_4
#define SNOOZE_SWITCH_GPIO GPIO_NUM_5  // スヌーズスイッチ用のGPIOピン
#define SET_BUTTON_GPIO GPIO_NUM_18    // 設定ボタン用のGPIOピン

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

static bool snooze_enabled = false;  // スヌーズ機能の状態

// 時計のアイコン用のカスタムキャラクター
uint8_t clock_char[8] = {
    0b00000,
    0b01110,
    0b10101,
    0b10111,
    0b10001,
    0b01110,
    0b00000,
    0b00000
};

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

// スヌーズ設定用の変数
static bool setting_mode = false;      // 設定モード中かどうか
static bool blink_state = false;       // 点滅状態
static uint8_t setting_target = 0;     // 設定対象（0:時、1:分、2:秒）
static uint8_t snooze_hours = 0;       // スヌーズ時間（時）
static uint8_t snooze_minutes = 0;     // スヌーズ時間（分）
static uint8_t snooze_seconds = 0;     // スヌーズ時間（秒）
static uint32_t last_button_press = 0; // 最後のボタン押下時刻
static bool button_pressed = false;    // ボタン押下状態

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

void init_snooze_switch(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SNOOZE_SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

void check_snooze_switch(void)
{
    static bool last_state = false;
    bool current_state = gpio_get_level(SNOOZE_SWITCH_GPIO) == 0;  // プルアップなので、0がON

    if (current_state != last_state) {
        snooze_enabled = current_state;
        last_state = current_state;
        
        // LCDの表示を更新
        lcd_send_cmd(0xC0 + 9);  // 1行目の先頭に移動
        if (snooze_enabled) {
            lcd_send_data(0x02);  // 時計アイコンを表示
        } else {
            lcd_send_data(' ');   // スペースで消去
        }
    }
}

void initialize_time()
{
    // Get the current time from an NTP server (Wi-Fi connection required)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    setenv("TZ", "JST-9", 1);
    tzset();

    init_snooze_switch();
    check_snooze_switch();
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

// ボタン初期化関数
void init_set_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

// ボタン処理関数
void handle_set_button(void)
{
    bool current_state = gpio_get_level(SET_BUTTON_GPIO) == 0;  // プルアップなので、0が押下
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (current_state && !button_pressed) {
        // ボタンが押された
        button_pressed = true;
        last_button_press = current_time;
    } else if (!current_state && button_pressed) {
        // ボタンが離された
        button_pressed = false;
        uint32_t press_duration = current_time - last_button_press;

        if (press_duration < 3000) {  // 3秒未満の押下
            if (!setting_mode) {
                // 設定モード開始
                setting_mode = true;
                setting_target = 0;
                // snooze_hours = 0;
                // snooze_minutes = 0;
                // snooze_seconds = 0;
            } else {
                // 現在の設定対象の値を増加
                switch (setting_target) {
                    case 0: snooze_hours = (snooze_hours + 1) % 24; break;
                    case 1: snooze_minutes = (snooze_minutes + 1) % 60; break;
                    case 2: snooze_seconds = (snooze_seconds + 1) % 60; break;
                }
            }
        } else {  // 3秒以上の長押し
            if (setting_mode) {
                setting_target++;
                if (setting_target > 2) {
                    // 設定完了
                    setting_mode = false;
                    setting_target = 0;
                }
            }
        }
    }
}

// スヌーズ時間チェック関数
void check_snooze_time(void)
{
    if (!snooze_enabled || !setting_mode) return;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_hour == snooze_hours &&
        timeinfo.tm_min == snooze_minutes &&
        timeinfo.tm_sec == snooze_seconds) {
        ESP_LOGI("SNOOZE", "It's time!");
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
    static uint32_t last_blink = 0;
    static bool time_reached = false;  // 時間到達フラグ

    while (1) {
        check_snooze_switch();
        handle_set_button();
        check_snooze_time();

        time(&now);
        localtime_r(&now, &timeinfo);

        // Retrieve both temperature and humidity from DHT11 in one call
        esp_err_t res = dht_read_data(DHT_TYPE_DHT11, DHT_GPIO, &humidity, &temperature);
        float temp = (res == ESP_OK) ? (float)temperature / 10.0 : -100.0;
        float hum = (res == ESP_OK) ? (float)humidity / 10.0 : -1.0;

        // デバッグ用のログ出力
        if (snooze_enabled && setting_mode) {
            ESP_LOGI("DEBUG", "Current: %02d:%02d:%02d, Snooze: %02d:%02d:%02d, Enabled: %d, Setting: %d",
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                     snooze_hours, snooze_minutes, snooze_seconds,
                     snooze_enabled, setting_mode);
        }

        // Line 1: Date and weekday
        snprintf(line1, sizeof(line1), "%04d%02d%02d %s %2.0f",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 weekdays[timeinfo.tm_wday], temp);

        // Line 2: Time and humidity
        if (setting_mode) {
            // 設定モード中の表示
            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (current_time - last_blink >= 200) {  // 200msごとに点滅
                blink_state = !blink_state;
                last_blink = current_time;
            }

            if (blink_state || setting_target != 0) {
                snprintf(line2, sizeof(line2), "%02d:", snooze_hours);
            } else {
                snprintf(line2, sizeof(line2), "  :");
            }
            if (blink_state || setting_target != 1) {
                char temp[5];
                snprintf(temp, sizeof(temp), "%02d:", snooze_minutes);
                strcat(line2, temp);
            } else {
                strcat(line2, "  :");
            }
            if (blink_state || setting_target != 2) {
                char temp[5];
                snprintf(temp, sizeof(temp), "%02d", snooze_seconds);
                strcat(line2, temp);
            } else {
                strcat(line2, "  ");
            }
            char temp[10];
            snprintf(temp, sizeof(temp), "     %2.0f%%", hum);
            strcat(line2, temp);
        } else {
            // 通常表示
            snprintf(line2, sizeof(line2), "%02d:%02d:%02d     %2.0f%%",
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                     hum);
        }

        lcd_send_cmd(0x80);                 // Set cursor to the beginning of line 1
        lcd_send_string(line1);
        lcd_send_data(0x00);                // Display character from slot 0 (degree Celsius symbol)
        lcd_send_cmd(0xC0);                 // Set cursor to the beginning of line 2
        lcd_send_string(line2);
        lcd_send_cmd(0xC0 + 12);            // Move cursor to position 12 of line 2
        lcd_send_data(0x01);                // Display character from slot 1 (humidity symbol)

        if(snooze_enabled){
            lcd_send_cmd(0xC0 + 9);
            lcd_send_data(0x02);
        }

        // スヌーズ時間のチェック（設定モード中は除外）
        if (snooze_enabled && !setting_mode) {
            if (timeinfo.tm_hour == snooze_hours &&
                timeinfo.tm_min == snooze_minutes &&
                timeinfo.tm_sec == snooze_seconds) {
                if (!time_reached) {  // 初回のみメッセージを表示
                    ESP_LOGI("SNOOZE", "It's time!");
                    time_reached = true;
                }
            } else {
                time_reached = false;  // 時間が異なる場合はフラグをリセット
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));    // Update every 200ms
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
    lcd_create_custom_char(2, clock_char);  // 時計アイコンをスロット2に登録

    initialize_time();         // Get the current time from the network
    setenv("TZ", "JST-9", 1);
    tzset();
    
    init_snooze_switch();
    init_set_button();
    check_snooze_switch();
    
    display_time_on_lcd();     // Display on the LCD
}