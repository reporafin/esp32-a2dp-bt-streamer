#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

static const char *TAG = "AUDIO_PLAYER";
#define MOUNT_POINT "/sdcard"

// Hardware Pins
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CLK  18
#define SD_CS   5

#define TFT_MOSI 13
#define TFT_CLK  14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4

// 5-Button Layout
#define BTN_PLAY  21
#define BTN_NEXT  22
#define BTN_PREV  25
#define BTN_VOL_U 26
#define BTN_VOL_D 27

// System & Playlist Variables
typedef enum {
    STATE_SCANNING,
    STATE_MUSIC_PLAYER
} app_state_t;
app_state_t current_state = STATE_SCANNING;

uint8_t current_volume = 10; 
bool is_playing = false;
bool a2dp_initialized = false;
bool request_next_track = false;

// Playlist System
#define MAX_TRACKS 20  // your tracks number
char playlist_paths[MAX_TRACKS][128];
char playlist_names[MAX_TRACKS][64];
int track_count = 0;
int current_track_idx = 0;
FILE *audio_file = NULL;

typedef struct {
    char name[32];
    uint8_t bda[6];
} bt_device_t;

// GLOBAL DEVICE MEMORY
bt_device_t known_devices[20];
int known_device_count = 0;

QueueHandle_t button_queue;
QueueHandle_t bt_device_queue;
StreamBufferHandle_t audio_stream_buf = NULL; 

// UI Elements
lv_group_t * device_list_group;
lv_obj_t *status_label;
lv_obj_t *volume_label;
lv_obj_t *main_list;
lv_obj_t *now_playing_label;
lv_obj_t *track_name_label;

// ISR & Audio Handlers
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(button_queue, &gpio_num, NULL);
}

void load_track(int index) {
    if (track_count == 0) return;
    if (audio_file) {
        fclose(audio_file);
        audio_file = NULL;
    }
    
    // Instantly flush the stream buffer so the old song clears out
    if (audio_stream_buf != NULL) {
        xStreamBufferReset(audio_stream_buf);
    }
    
    if (strstr(playlist_paths[index], ".wav")) {
        audio_file = fopen(playlist_paths[index], "rb");
        if (audio_file) fseek(audio_file, 44, SEEK_SET); 
    }
    ESP_LOGI(TAG, "Loaded track: %s", playlist_names[index]);
}

// Background : fills the 32KB RAM buffer from the SD Card
void audio_reader_task(void *pvParameter) {
    uint8_t read_buf[2048];
    while (1) {
        if (is_playing && audio_file != NULL) {
            if (xStreamBufferSpacesAvailable(audio_stream_buf) >= sizeof(read_buf)) {
                size_t bytes_read = fread(read_buf, 1, sizeof(read_buf), audio_file);
                if (bytes_read > 0) {
                    xStreamBufferSend(audio_stream_buf, read_buf, bytes_read, portMAX_DELAY);
                } else {
                    request_next_track = true;
                    vTaskDelay(pdMS_TO_TICKS(500)); 
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10)); 
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50)); 
        }
    }
}

// A2DP : pulls audio from the RAM buffer
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len) {
    if (len <= 0 || data == NULL || !is_playing) {
        if (len > 0) memset(data, 0, len); 
        return len;
    }
    
    int16_t *pcm = (int16_t *)data;
    int samples = len / 4; 

    if (audio_file != NULL) {
        size_t bytes_read = xStreamBufferReceive(audio_stream_buf, data, len, 0);
        if (bytes_read < len) {
            memset(data + bytes_read, 0, len - bytes_read);
        }
    } else {
        static uint32_t phase = 0;
        for (int i = 0; i < samples; i++) {
            int16_t val = (phase++ % 100 < 50) ? 3000 : -3000;
            pcm[2 * i] = val;       
            pcm[2 * i + 1] = val;   
        }
    }

    for (int i = 0; i < len / 2; i++) {
        pcm[i] = (pcm[i] * current_volume) / 100;
    }
    return len;
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP CONNECTED! Starting audio stream...");
            load_track(current_track_idx);
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            is_playing = true;
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP Disconnected.");
            if (audio_file) { fclose(audio_file); audio_file = NULL; }
            is_playing = false;
        }
    }
}

// SD Card & UI
void mount_and_read_sd(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI, .miso_io_num = SD_MISO, .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 10000; 
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = SPI3_HOST;

    if (esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card) == ESP_OK) {
        DIR *dir = opendir(MOUNT_POINT);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && track_count < MAX_TRACKS) {
                if (entry->d_type == DT_REG && (strstr(entry->d_name, ".wav") || strstr(entry->d_name, ".mp3"))) {
                    strcpy(playlist_paths[track_count], MOUNT_POINT);
                    strcat(playlist_paths[track_count], "/");
                    strcat(playlist_paths[track_count], entry->d_name);
                    strcpy(playlist_names[track_count], entry->d_name);
                    track_count++;
                }
            }
            closedir(dir);
        }
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_flush_ready((lv_disp_drv_t *)user_ctx); 
    return false;
}

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    uint32_t size = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    uint16_t *color_p = (uint16_t *)color_map;
    for (uint32_t i = 0; i < size; i++) color_p[i] = (color_p[i] >> 8) | (color_p[i] << 8);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

void build_initial_ui(void) {
    lv_obj_t *status_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(status_bar, LV_PCT(100), 30);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    
    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "Scanning BT...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 5, 0);

    volume_label = lv_label_create(status_bar);
    lv_label_set_text_fmt(volume_label, "Vol: %d%%", current_volume); 
    lv_obj_set_style_text_color(volume_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(volume_label, LV_ALIGN_RIGHT_MID, -5, 0);

    main_list = lv_list_create(lv_scr_act());
    lv_obj_set_size(main_list, LV_PCT(100), LV_PCT(80));
    lv_obj_align(main_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(main_list, lv_color_hex(0x1E1E1E), 0);

    device_list_group = lv_group_create();
}

// Button config
void button_task(void *pvParameter) {
    uint32_t btn_pin;
    while (1) {
        if (xQueueReceive(button_queue, &btn_pin, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(btn_pin) == 0) {
                
                if (btn_pin == BTN_VOL_U) {
                    if (current_volume <= 95) current_volume += 5;
                    else current_volume = 100;
                    lv_label_set_text_fmt(volume_label, "Vol: %d%%", current_volume);
                } 
                else if (btn_pin == BTN_VOL_D) {
                    if (current_volume >= 5) current_volume -= 5;
                    else current_volume = 0;
                    lv_label_set_text_fmt(volume_label, "Vol: %d%%", current_volume);
                }
                
                // Unified Navigation: NEXT button moves down the list or skips track
                else if (btn_pin == BTN_NEXT) {
                    if (current_state == STATE_SCANNING) {
                        lv_group_focus_next(device_list_group);
                    } else if (current_state == STATE_MUSIC_PLAYER && track_count > 0) {
                        current_track_idx = (current_track_idx + 1) % track_count;
                        load_track(current_track_idx);
                        lv_label_set_text_fmt(track_name_label, "Playing: %d/%d\n%s", current_track_idx + 1, track_count, playlist_names[current_track_idx]);
                    }
                }
                
                // Navigation: PREV button moves up the list or goes back a track
                else if (btn_pin == BTN_PREV) {
                    if (current_state == STATE_SCANNING) {
                        lv_group_focus_prev(device_list_group);
                    } else if (current_state == STATE_MUSIC_PLAYER && track_count > 0) {
                        current_track_idx = (current_track_idx - 1 + track_count) % track_count;
                        load_track(current_track_idx);
                        lv_label_set_text_fmt(track_name_label, "Playing: %d/%d\n%s", current_track_idx + 1, track_count, playlist_names[current_track_idx]);
                    }
                }
                
                else if (btn_pin == BTN_PLAY) {
                    if (current_state == STATE_MUSIC_PLAYER) {
                        if (is_playing) {
                            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                            lv_label_set_text(status_label, "Paused");
                        } else {
                            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                            lv_label_set_text(status_label, "Connected!");
                        }
                        is_playing = !is_playing;
                    } 
                    else if (current_state == STATE_SCANNING) {
                        lv_obj_t * focused_btn = lv_group_get_focused(device_list_group);
                        if (focused_btn != NULL) {
                            const char * selected_text = lv_list_get_btn_text(main_list, focused_btn);
                            
                            int target_idx = -1;
                            for (int i = 0; i < known_device_count; i++) {
                                if (strcmp(known_devices[i].name, selected_text) == 0) {
                                    target_idx = i;
                                    break;
                                }
                            }

                            if (target_idx != -1) {
                                lv_label_set_text(status_label, "Connecting...");
                                lv_obj_clean(main_list);
                                
                                now_playing_label = lv_label_create(main_list);
                                lv_label_set_text(now_playing_label, LV_SYMBOL_AUDIO " MUSIC PLAYER");
                                lv_obj_set_style_text_color(now_playing_label, lv_color_hex(0xFFFFFF), 0);
                                lv_obj_align(now_playing_label, LV_ALIGN_TOP_MID, 0, 20);
                                
                                track_name_label = lv_label_create(main_list);
                                if (track_count > 0) {
                                    lv_label_set_text_fmt(track_name_label, "Playing: %d/%d\n%s", current_track_idx + 1, track_count, playlist_names[current_track_idx]);
                                } else {
                                    lv_label_set_text(track_name_label, "No Tracks Found");
                                }
                                lv_obj_set_style_text_color(track_name_label, lv_color_hex(0x2196F3), 0);
                                lv_obj_align(track_name_label, LV_ALIGN_CENTER, 0, 0);

                                current_state = STATE_MUSIC_PLAYER;
                                
                                esp_bd_addr_t target_bda;
                                memcpy(target_bda, known_devices[target_idx].bda, 6);
                                
                                esp_bt_gap_cancel_discovery();
                                if (!a2dp_initialized) {
                                    esp_a2d_register_callback(bt_app_a2d_cb);
                                    esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);
                                    esp_a2d_source_init();
                                    a2dp_initialized = true;
                                }
                                ESP_LOGI(TAG, "Initiating handshake with speaker: %s", known_devices[target_idx].name);
                                esp_a2d_source_connect(target_bda);
                            }
                        }
                    }
                } 
            }
        }
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        char *device_name = "Unknown Device";
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                device_name = (char *)param->disc_res.prop[i].val;
                break;
            }
        }
        bt_device_t new_device;
        memcpy(new_device.bda, param->disc_res.bda, 6);
        if (strcmp(device_name, "Unknown Device") == 0) {
            snprintf(new_device.name, sizeof(new_device.name), "%02x:%02x:%02x:%02x:%02x:%02x", 
                     param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                     param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
        } else {
            strncpy(new_device.name, device_name, sizeof(new_device.name) - 1);
            new_device.name[sizeof(new_device.name) - 1] = '\0';
        }
        xQueueSend(bt_device_queue, &new_device, 0); 
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && current_state == STATE_SCANNING) {
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        }
    }
}

void app_main(void) {
    button_queue = xQueueCreate(10, sizeof(uint32_t));
    bt_device_queue = xQueueCreate(15, sizeof(bt_device_t));

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL<<BTN_PLAY)|(1ULL<<BTN_NEXT)|(1ULL<<BTN_PREV)|(1ULL<<BTN_VOL_U)|(1ULL<<BTN_VOL_D),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    
    gpio_isr_handler_add(BTN_PLAY, button_isr_handler, (void*) BTN_PLAY);
    gpio_isr_handler_add(BTN_NEXT, button_isr_handler, (void*) BTN_NEXT);
    gpio_isr_handler_add(BTN_PREV, button_isr_handler, (void*) BTN_PREV);
    gpio_isr_handler_add(BTN_VOL_U, button_isr_handler, (void*) BTN_VOL_U);
    gpio_isr_handler_add(BTN_VOL_D, button_isr_handler, (void*) BTN_VOL_D);

    mount_and_read_sd();

    spi_bus_config_t tft_buscfg = { .sclk_io_num = TFT_CLK, .mosi_io_num = TFT_MOSI, .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 320*240*2 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &tft_buscfg, SPI_DMA_CH_AUTO));

    static lv_disp_drv_t disp_drv; 
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = { .dc_gpio_num = TFT_DC, .cs_gpio_num = TFT_CS, .pclk_hz = 20 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10, .on_color_trans_done = notify_lvgl_flush_ready, .user_ctx = &disp_drv };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = { .reset_gpio_num = TFT_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(100)); 
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 20)); 
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lv_init();
    lv_color_t *buf1 = heap_caps_malloc(240 * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(240 * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, 240 * 20);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240; disp_drv.ver_res = 280; disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &disp_buf; disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    build_initial_ui();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    audio_stream_buf = xStreamBufferCreate(32 * 1024, 1);
    xTaskCreate(audio_reader_task, "audio_reader", 4096, NULL, 5, NULL);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(bt_cfg.mode)); 
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_app_gap_cb));
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    while (1) {
        // no Auto-Skipping when a track finishes
        if (request_next_track) {
            request_next_track = false;
            if (track_count > 0 && current_state == STATE_MUSIC_PLAYER) {
                current_track_idx = (current_track_idx + 1) % track_count;
                load_track(current_track_idx);
                lv_label_set_text_fmt(track_name_label, "Playing: %d/%d\n%s", current_track_idx + 1, track_count, playlist_names[current_track_idx]);
            }
        }

        bt_device_t received_device;
        if (xQueueReceive(bt_device_queue, &received_device, 0)) {
            bool is_duplicate = false;
            for (int i = 0; i < known_device_count; i++) {
                if (memcmp(known_devices[i].bda, received_device.bda, 6) == 0) { is_duplicate = true; break; }
            }
            if (!is_duplicate && known_device_count < 20) {
                memcpy(known_devices[known_device_count].bda, received_device.bda, 6);
                strncpy(known_devices[known_device_count].name, received_device.name, 32);
                known_device_count++;
                lv_obj_t * btn = lv_list_add_btn(main_list, LV_SYMBOL_BLUETOOTH, received_device.name);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x2196F3), LV_STATE_FOCUSED);
                lv_obj_set_style_bg_opa(btn, 255, LV_STATE_FOCUSED);
                lv_group_add_obj(device_list_group, btn); 
            }
        }
        lv_timer_handler();
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}