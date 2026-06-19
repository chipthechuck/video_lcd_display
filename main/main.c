/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "app_video.h"
#include "app_lcd.h"

#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len, void *user_data);

static const char *TAG = "app_main";

#define EXAMPLE_CAMERA_RGB_SWAP             (0)
#define EXAMPLE_CAMERA_ZERO_COPY_TO_LCD     (1)
#define EXAMPLE_ENABLE_TEXT_OVERLAY         (1)
#define EXAMPLE_OVERLAY_TEXT                "CAMERA"
#define EXAMPLE_OVERLAY_TEXT_SCALE          (4)
#define EXAMPLE_OVERLAY_TEXT_ALPHA          (220)
#define EXAMPLE_OVERLAY_BLEND_FULL_FRAME    (1)

static esp_lcd_panel_handle_t display_panel;
static ppa_client_handle_t ppa_srm_handle = NULL;
#if EXAMPLE_ENABLE_TEXT_OVERLAY
static ppa_client_handle_t ppa_blend_handle = NULL;
#endif
static size_t data_cache_line_size = 0;
static void *lcd_buffer[EXAMPLE_LCD_BUF_NUM];
static bool camera_uses_lcd_buffer;
#if EXAMPLE_ENABLE_TEXT_OVERLAY
static uint8_t *overlay_text_buffer;
static size_t overlay_text_buffer_size;
static uint32_t overlay_text_w;
static uint32_t overlay_text_h;
#endif

#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
static int fps_count;
static int64_t start_time;
#endif

#if EXAMPLE_ENABLE_TEXT_OVERLAY
static const char *get_overlay_glyph(char c)
{
    switch (c) {
    case 'A':
        return "01110"
               "10001"
               "10001"
               "11111"
               "10001"
               "10001"
               "10001";
    case 'C':
        return "01110"
               "10001"
               "10000"
               "10000"
               "10000"
               "10001"
               "01110";
    case 'E':
        return "11111"
               "10000"
               "10000"
               "11110"
               "10000"
               "10000"
               "11111";
    case 'M':
        return "10001"
               "11011"
               "10101"
               "10101"
               "10001"
               "10001"
               "10001";
    case 'R':
        return "11110"
               "10001"
               "10001"
               "11110"
               "10100"
               "10010"
               "10001";
    default:
        return "00000"
               "00000"
               "00000"
               "00000"
               "00000"
               "00000"
               "00000";
    }
}

static void app_overlay_text_init(void)
{
    const char *text = EXAMPLE_OVERLAY_TEXT;
    const uint32_t text_len = strlen(text);
    const uint32_t glyph_w = 5;
    const uint32_t glyph_h = 7;
    const uint32_t glyph_gap = 1;

    const uint32_t text_w = ((glyph_w + glyph_gap) * text_len - glyph_gap) * EXAMPLE_OVERLAY_TEXT_SCALE;
    const uint32_t text_h = glyph_h * EXAMPLE_OVERLAY_TEXT_SCALE;

#if EXAMPLE_OVERLAY_BLEND_FULL_FRAME
    overlay_text_w = EXAMPLE_LCD_H_RES;
    overlay_text_h = EXAMPLE_LCD_V_RES;
#else
    overlay_text_w = text_w;
    overlay_text_h = text_h;
#endif

    const uint32_t text_x = (overlay_text_w - text_w) / 2;
    const uint32_t text_y = (overlay_text_h - text_h) / 2;

    overlay_text_buffer_size = ALIGN_UP(overlay_text_w * overlay_text_h, data_cache_line_size);
    overlay_text_buffer = heap_caps_aligned_calloc(data_cache_line_size, 1, overlay_text_buffer_size,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(overlay_text_buffer ? ESP_OK : ESP_ERR_NO_MEM);

    for (uint32_t i = 0; i < text_len; i++) {
        const char *glyph = get_overlay_glyph(text[i]);
        const uint32_t glyph_x = text_x + i * (glyph_w + glyph_gap) * EXAMPLE_OVERLAY_TEXT_SCALE;

        for (uint32_t y = 0; y < glyph_h; y++) {
            for (uint32_t x = 0; x < glyph_w; x++) {
                if (glyph[y * glyph_w + x] != '1') {
                    continue;
                }

                const uint32_t pixel_x = glyph_x + x * EXAMPLE_OVERLAY_TEXT_SCALE;
                const uint32_t pixel_y = text_y + y * EXAMPLE_OVERLAY_TEXT_SCALE;
                for (uint32_t sy = 0; sy < EXAMPLE_OVERLAY_TEXT_SCALE; sy++) {
                    memset(&overlay_text_buffer[(pixel_y + sy) * overlay_text_w + pixel_x],
                           EXAMPLE_OVERLAY_TEXT_ALPHA, EXAMPLE_OVERLAY_TEXT_SCALE);
                }
            }
        }
    }

    ESP_ERROR_CHECK(esp_cache_msync(overlay_text_buffer, overlay_text_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
}

static void app_blend_text_overlay(void *frame_buffer, uint8_t buffer_index, uint32_t frame_w, uint32_t frame_h)
{
    if (!overlay_text_buffer || overlay_text_w > frame_w || overlay_text_h > frame_h) {
        return;
    }

    const uint32_t overlay_x = (frame_w - overlay_text_w) / 2;
    const uint32_t overlay_y = (frame_h - overlay_text_h) / 2;
    const uint32_t frame_buffer_size = ALIGN_UP(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES *
                                                (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3),
                                                data_cache_line_size);

    ppa_blend_oper_config_t blend_config = {
        .in_bg.buffer = frame_buffer,
        .in_bg.pic_w = frame_w,
        .in_bg.pic_h = frame_h,
        .in_bg.block_w = overlay_text_w,
        .in_bg.block_h = overlay_text_h,
        .in_bg.block_offset_x = overlay_x,
        .in_bg.block_offset_y = overlay_y,
        .in_bg.blend_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_BLEND_COLOR_MODE_RGB565 : PPA_BLEND_COLOR_MODE_RGB888,
        .in_fg.buffer = overlay_text_buffer,
        .in_fg.pic_w = overlay_text_w,
        .in_fg.pic_h = overlay_text_h,
        .in_fg.block_w = overlay_text_w,
        .in_fg.block_h = overlay_text_h,
        .in_fg.block_offset_x = 0,
        .in_fg.block_offset_y = 0,
        .in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8,
        .out.buffer = lcd_buffer[buffer_index],
        .out.buffer_size = frame_buffer_size,
        .out.pic_w = frame_w,
        .out.pic_h = frame_h,
        .out.block_offset_x = overlay_x,
        .out.block_offset_y = overlay_y,
        .out.blend_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_BLEND_COLOR_MODE_RGB565 : PPA_BLEND_COLOR_MODE_RGB888,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_fix_rgb_val = {
            .b = 0xff,
            .g = 0xff,
            .r = 0xff,
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    ESP_ERROR_CHECK(ppa_do_blend(ppa_blend_handle, &blend_config));
}
#endif

void app_main(void)
{
    // Initialize the LCD
    ESP_ERROR_CHECK(app_lcd_init(&display_panel));

    // Initialize the PPA
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &data_cache_line_size));
#if EXAMPLE_ENABLE_TEXT_OVERLAY
    ppa_client_config_t ppa_blend_config = {
        .oper_type = PPA_OPERATION_BLEND,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_blend_config, &ppa_blend_handle));
    app_overlay_text_init();
#endif

    // Initialize the video camera
    esp_err_t ret = app_video_main(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video main init failed with error 0x%x", ret);
        return;
    }

    // Open the video device
    int video_cam_fd0 = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    // Get the LCD frame buffer
#if EXAMPLE_LCD_BUF_NUM == 2
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_panel, 2, &lcd_buffer[0], &lcd_buffer[1]));
#else
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_panel, 3, &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));
#endif

    // Set the video buffer
#if CONFIG_EXAMPLE_USE_MEMORY_MAPPING
    ESP_LOGI(TAG, "Using map buffer");
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, NULL)); // When setting the camera video buffer, it can be written as NULL to automatically allocate the buffer using mapping
#else
    ESP_LOGI(TAG, "Using user defined buffer");
    void *camera_buf[EXAMPLE_CAM_BUF_NUM];
    uint32_t camera_h_res = 0;
    uint32_t camera_v_res = 0;
    const size_t lcd_buffer_size = ALIGN_UP(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (EXAMPLE_LCD_BIT_PER_PIXEL / 8), data_cache_line_size);
    const size_t camera_buffer_size = app_video_get_buf_size();
    app_video_get_frame_size(&camera_h_res, &camera_v_res);
    const bool camera_matches_lcd = camera_h_res == EXAMPLE_LCD_H_RES &&
                                    camera_v_res == EXAMPLE_LCD_V_RES &&
                                    camera_buffer_size <= lcd_buffer_size;

    if (EXAMPLE_CAMERA_ZERO_COPY_TO_LCD && !EXAMPLE_CAMERA_RGB_SWAP && camera_matches_lcd) {
        ESP_LOGI(TAG, "Using LCD frame buffers as camera buffers");
        camera_uses_lcd_buffer = true;
        ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, (const void **)lcd_buffer));
    } else {
        camera_uses_lcd_buffer = false;
        ESP_LOGW(TAG, "Using dedicated camera buffers (camera=%lux%lu/%u bytes, LCD=%ux%u/%u bytes, rgb_swap=%d, zero_copy=%d, size_match=%d)",
                 (unsigned long)camera_h_res, (unsigned long)camera_v_res, (unsigned int)camera_buffer_size,
                 EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, (unsigned int)lcd_buffer_size,
                 EXAMPLE_CAMERA_RGB_SWAP, EXAMPLE_CAMERA_ZERO_COPY_TO_LCD, camera_matches_lcd);

        for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
            camera_buf[i] = heap_caps_aligned_calloc(data_cache_line_size, 1, camera_buffer_size,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (camera_buf[i] == NULL) {
                ESP_LOGE(TAG, "failed to allocate camera buffer");
                return;
            }
        }
        ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, (const void **)camera_buf));
    }
#endif

    // Register the video frame operation callback
    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_operation));

    // Start the camera stream task
#if CONFIG_FREERTOS_UNICORE
    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 0, NULL));
#else
    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 1, NULL));
#endif

#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
    start_time = esp_timer_get_time();  // Get the initial time for frame rate statistics
#endif
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len, void *user_data)
{
#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
    fps_count++;
    if (fps_count == 50) {
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
        start_time = end_time;
        fps_count = 0;

        ESP_LOGI(TAG, "camera_buf_hes: %lu, camera_buf_ves: %lu, camera_buf_len: %d KB", camera_buf_hes, camera_buf_ves, camera_buf_len / 1024);
    }
#endif

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = camera_buf_hes,
        .in.block_h = camera_buf_ves,
        .in.block_offset_x = (camera_buf_hes > EXAMPLE_LCD_H_RES) ? (camera_buf_hes - EXAMPLE_LCD_H_RES) / 2 : 0,
        .in.block_offset_y = (camera_buf_ves > EXAMPLE_LCD_V_RES) ? (camera_buf_ves - EXAMPLE_LCD_V_RES) / 2 : 0,
        .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,
        .out.buffer = lcd_buffer[camera_buf_index],
        .out.buffer_size = ALIGN_UP(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3), data_cache_line_size),
        .out.pic_w = EXAMPLE_LCD_H_RES,
        .out.pic_h = EXAMPLE_LCD_V_RES,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = EXAMPLE_CAMERA_RGB_SWAP,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    if (EXAMPLE_CAMERA_RGB_SWAP || camera_buf_hes > EXAMPLE_LCD_H_RES || camera_buf_ves > EXAMPLE_LCD_V_RES) {
        srm_config.in.block_w = (camera_buf_hes > EXAMPLE_LCD_H_RES) ? EXAMPLE_LCD_H_RES : camera_buf_hes;
        srm_config.in.block_h = (camera_buf_ves > EXAMPLE_LCD_V_RES) ? EXAMPLE_LCD_V_RES : camera_buf_ves;

        static uint32_t perf_count;
        static int64_t scale_us_total;
        static int64_t overlay_us_total;
        static int64_t draw_us_total;
        int64_t perf_start_us = esp_timer_get_time();

        if (EXAMPLE_CAMERA_RGB_SWAP) {
            ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config));
        } else {
            const uint32_t bytes_per_pixel = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3;
            const uint32_t copy_w_bytes = srm_config.in.block_w * bytes_per_pixel;
            const uint32_t src_stride = camera_buf_hes * bytes_per_pixel;
            const uint32_t dst_stride = EXAMPLE_LCD_H_RES * bytes_per_pixel;
            const uint8_t *src = camera_buf +
                                 srm_config.in.block_offset_y * src_stride +
                                 srm_config.in.block_offset_x * bytes_per_pixel;
            uint8_t *dst = lcd_buffer[camera_buf_index];

            if (copy_w_bytes == src_stride && copy_w_bytes == dst_stride) {
                memcpy(dst, src, copy_w_bytes * srm_config.in.block_h);
            } else {
                for (uint32_t y = 0; y < srm_config.in.block_h; y++) {
                    memcpy(dst, src, copy_w_bytes);
                    src += src_stride;
                    dst += dst_stride;
                }
            }
        }
        int64_t scale_done_us = esp_timer_get_time();
#if EXAMPLE_ENABLE_TEXT_OVERLAY
        app_blend_text_overlay(lcd_buffer[camera_buf_index], camera_buf_index, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
#endif
        int64_t overlay_done_us = esp_timer_get_time();

        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(display_panel, 0, 0, srm_config.in.block_w, srm_config.in.block_h, lcd_buffer[camera_buf_index]));
        int64_t draw_done_us = esp_timer_get_time();

        scale_us_total += scale_done_us - perf_start_us;
        overlay_us_total += overlay_done_us - scale_done_us;
        draw_us_total += draw_done_us - overlay_done_us;
        perf_count++;

        if (perf_count == 50) {
            ESP_LOGI(TAG, "perf avg us: scale=%lld, overlay=%lld, draw=%lld",
                     (long long)(scale_us_total / perf_count),
                     (long long)(overlay_us_total / perf_count),
                     (long long)(draw_us_total / perf_count));
            perf_count = 0;
            scale_us_total = 0;
            overlay_us_total = 0;
            draw_us_total = 0;
        }
    } else {
// #if EXAMPLE_ENABLE_TEXT_OVERLAY
//         if (camera_buf != lcd_buffer[camera_buf_index]) {
//             app_blend_text_overlay(camera_buf, camera_buf_index, camera_buf_hes, camera_buf_ves);
//             camera_buf = lcd_buffer[camera_buf_index];
//         }
// #endif
//         if (!camera_uses_lcd_buffer) {
//             ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(display_panel, 0, 0, camera_buf_hes, camera_buf_ves, camera_buf));
//         }
    }
}
