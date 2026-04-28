/* --------------------------------------------------------------
   Application: 06 
   Class: Real Time Systems - Spring 2026
   Author: R. Zephyr 
   Email: 
   Company: [University of Central Florida]     
---------------------------------------------------------------*/


#define MAX_COUNT_SEM 10
#define SENSOR_THRESHOLD 100
volatile int SEMCNT = 0;


#include <stdio.h>
#include <stdlib.h>
#include <string.h> //Requires by memset
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/adc.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <esp_http_server.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/api.h>
#include <lwip/netdb.h>


#define DEBUG_PIN_SENSOR GPIO_NUM_0 
#define LED_GREEN GPIO_NUM_5
#define Alert_LED   GPIO_NUM_4
#define BUTTON_PIN GPIO_NUM_18
#define POT_ADC_CHANNEL ADC1_CHANNEL_6
#define MAX_COUNT_SEM 10
#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define SAMPLES_PER_5_MIN 15000
volatile int current_bpm = 0;
volatile char current_status[32] = "NORMAL";
SemaphoreHandle_t sem_button;
SemaphoreHandle_t sem_sensor;
SemaphoreHandle_t print_mutex;

int peak_bpm = 0, low_bpm = 200;
int avg_bpm_last_min = 0;
int tachycardia_seconds = 0;
int bradycardia_seconds = 0;
bool nurse_call_active = false;


esp_err_t nurse_call_handler(httpd_req_t *req);

// AI made this HTML/Javascript
char web_page[] = 
"<!DOCTYPE html><html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>"
"body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; padding: 20px; background-color: #eef2f5; color: #333; }"
".card { padding: 20px; margin: 15px auto; border-radius: 15px; background: white; box-shadow: 0 6px 12px rgba(0,0,0,0.1); max-width: 450px; }"
".status { font-size: 22px; font-weight: bold; color: #2c3e50; margin: 5px 0; padding: 5px; border-radius: 5px; }"
".bpm { font-size: 64px; color: #e74c3c; font-weight: bold; margin: 10px 0; }"
".btn { padding: 15px 30px; font-size: 18px; background: #c0392b; color: white; border: none; border-radius: 50px; cursor: pointer; transition: 0.3s; font-weight: bold; text-transform: uppercase; }"
".btn:hover { background: #ff0000; box-shadow: 0 0 15px rgba(255,0,0,0.4); }"
".log-entry { text-align: left; background: #f9f9f9; padding: 10px; margin-top: 10px; border-left: 4px solid #3498db; display: none; }"
".log-title { font-weight: bold; color: #2980b9; text-transform: uppercase; font-size: 14px; }"
".log-val { float: right; font-weight: bold; color: #333; }"
"</style>"
"</head>"
"<body>"
    "<div class='card'>"
        "<h2>Vital Signs Monitor</h2>"
        "<div id='bpm_val' class='bpm'>--</div>"
        "<div id='status_val' class='status'>INITIALIZING...</div>"
        "<p style='color:#7f8c8d;'>Real-time Beats Per Minute</p>"
    "</div>"

    "<div id='nurse_log' class='card' style='display:none;'>"
        "<h3 style='color:#d35400;'> EMERGENCY CLINICAL LOG</h3>"
        "<div class='log-entry' style='display:block;'><span class='log-title'>1-Min Avg:</span> <span id='avg_val' class='log-val'>--</span></div>"
        "<div class='log-entry' style='display:block;'><span class='log-title'>Peak BPM:</span> <span id='peak_val' class='log-val'>--</span></div>"
        "<div class='log-entry' style='display:block;'><span class='log-title'>Low BPM:</span> <span id='low_val' class='log-val'>--</span></div>"
        "<div class='log-entry' style='display:block;'><span class='log-title'>Tachycardia (5 min):</span> <span id='tachy_time' class='log-val'>--</span></div>"
        "<div class='log-entry' style='display:block;'> <span class='log-title'>Bradycardia (5 min):</span> <span id='brady_time' class='log-val'>--</span> </div>"
    "</div>"

    "<div class='card'>"
        "<h3>Assistance</h3>"
        "<a href='/nursecall'><button class='btn'>NURSE CALL</button></a>"
    "</div>"

    "<script>"
    "setInterval(function() {"
    "  fetch('/data').then(response => response.json()).then(data => {"
         // Update Live Monitor
    "    document.getElementById('bpm_val').innerHTML = data.bpm;"
    "    document.getElementById('status_val').innerHTML = data.status;"
    "    document.getElementById('status_val').style.color = (data.bpm > 100 || data.bpm < 60) ? '#e74c3c' : '#27ae60';"

         // Conditional Log Update (Snapshot)
    "    if (data.nurse_alert) {"
    "      document.getElementById('nurse_log').style.display = 'block';"
    "      document.getElementById('avg_val').innerHTML = data.avg + ' BPM';"
    "      document.getElementById('peak_val').innerHTML = data.peak + ' BPM';"
    "      document.getElementById('low_val').innerHTML = data.low + ' BPM';"
    "      document.getElementById('tachy_time').innerHTML = data.tachy_sec + 's';"
    "      document.getElementById('brady_time').innerHTML = data.brady_sec + 's';"
    "    }"
    "  }).catch(err => console.error('Telemetry Error:', err));"
    "}, 1000);"
    "</script>"
"</body></html>";




//static const char *TAG = "System-Setup:"; // TAG for debug
int led_state = 0;

#define EXAMPLE_ESP_WIFI_SSID "Wokwi-GUEST"
#define EXAMPLE_ESP_WIFI_PASS ""
#define EXAMPLE_ESP_MAXIMUM_RETRY 5

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            printf("retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        printf("connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        printf( "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void connect_wifi(void){
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
           // .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        printf( "connected to ap SSID:%s password:%s", EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        printf( "Failed to connect to SSID:%s, password:%s", EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        printf( "UNEXPECTED EVENT");
    }
    vEventGroupDelete(s_wifi_event_group);
}

void replace_str(char *buf, const char *key, const char *val) {
    static char temp[4500]; // Static moves this out of the stack
    char *pos = strstr(buf, key);
    if (!pos) return;

    int before = pos - buf;
    int key_len = strlen(key);

    // Build the new string in temp
    strncpy(temp, buf, before);
    temp[before] = '\0';
    strcat(temp, val);
    strcat(temp, pos + key_len);

    // Copy back to the main buffer
    strcpy(buf, temp);
}

esp_err_t send_web_page(httpd_req_t *req)
{   
    static char page[4500]; 
    
    // Reset and copy the template
    memset(page, 0, sizeof(page));
    strcpy(page, web_page);

    char bpm_str[16];
    sprintf(bpm_str, "%d", current_bpm);

    // Protect shared data while building the string
    xSemaphoreTake(print_mutex, portMAX_DELAY);
    replace_str(page, "%STATUS%", (char*)current_status);
    replace_str(page, "%BPM%", bpm_str);
    xSemaphoreGive(print_mutex);

    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    
}
esp_err_t get_req_handler(httpd_req_t *req)
{
    return send_web_page(req);
}

esp_err_t get_data_handler(httpd_req_t *req)
{
    char json_data[1024];
    // Formats data as {"bpm": 72, "status": "NORMAL"}

    char live_section[512];
    sprintf(live_section, "\"bpm\": %d, \"status\": \"%s\"", current_bpm, current_status);
    
    
    char log_section[256] = "";
    if (nurse_call_active) {
        sprintf(log_section, 
            ", \"avg\": %d, \"peak\": %d, \"low\": %d, \"tachy_sec\": %d,\"brady_sec\": %d, \"nurse_alert\": true",
            avg_bpm_last_min, peak_bpm, low_bpm, tachycardia_seconds,bradycardia_seconds);
    } else {
        sprintf(log_section, ", \"nurse_alert\": false");
    }
    
    // Combine into final JSON
    snprintf(json_data, sizeof(json_data), "{%s%s}", live_section, log_section);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_data, HTTPD_RESP_USE_STRLEN);
}

// AI was used to help alter this part of the Server set up and website controls

httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = get_req_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_nurse = {
    .uri = "/nursecall",
    .method = HTTP_GET,
    .handler = nurse_call_handler,
};

httpd_uri_t uri_data = {
    .uri = "/data",
    .method = HTTP_GET,
    .handler = get_data_handler,
    .user_ctx = NULL
};


esp_err_t nurse_call_handler(httpd_req_t *req)
{
    nurse_call_active = true;
    xSemaphoreGive(sem_button);

    xSemaphoreTake(print_mutex, portMAX_DELAY);
    printf("[WEB] Emergency Nurse Call triggered from Browser!\n");
    xSemaphoreGive(print_mutex);

    httpd_resp_send(req, "<h1>Nurse Alerted</h1><p>Returning to monitor...</p><script>setTimeout(() => {window.location.href='/'}, 2000);</script>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}    

httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0; // pins the HTML delivery logic to Core 0
    
    config.max_uri_handlers     = 8;
    config.max_resp_headers     = 8;
   
    httpd_handle_t server = NULL;


    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_nurse);
        httpd_register_uri_handler(server, &uri_data);
    }

    return server;
}



// Task: heartbeat_task
// Company Role: System Alive Indicator
// Period: 1000 ms
// Deadline: Soft Real-Time
// Priority: Low (1)
// Purpose: Blinks green LED to prove scheduler is alive
void heartbeat_task(void *pvParameters) {
    while (1) {
        gpio_set_level(LED_GREEN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));

        gpio_set_level(LED_GREEN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        //printf("[Heartbeat] %lu ms\n", xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
}
// TASK: sensor_task
// Company Role: Bedside Patient Vital Sign Monitor
// Period: 20 ms
// Deadline: HARD Real-Time
// Priority: High (3)
// Purpose: Samples ADC heart-rate sensor and detects tachycardia / bradycardia conditions.
// Uses vTaskDelayUntil() for deterministic timing.
void sensor_task(void *pvParameters) {
    
    long bpm_sum = 0;
    int sample_count = 0;
    int state_counter = 0; // Increments every sample to track time
    int total_window_samples = 0;
    
    static bool alert_active = false;
    TickType_t lastWakeTime = xTaskGetTickCount();
    static int print_counter = 0;

    while (1) {
        gpio_set_level(DEBUG_PIN_SENSOR, 1);
        int val = adc1_get_raw(POT_ADC_CHANNEL);
        int bpm = (val * 140 / 4095) + 40;
        char* status = "NORMAL";
        
        //Track Peaks and Lows
        if (bpm > peak_bpm) peak_bpm = bpm;
        if (bpm < low_bpm) low_bpm = bpm;
        //Accumulate for Average (3000 samples = 1 minute)
        bpm_sum += bpm;
        sample_count++;

        // Determine Status for the periodic log
        if (bpm > 100) {
            status = "!! TACHYCARDIA !!";
        } else if (bpm < 60) {
            status = "!! BRADYCARDIA !!";
        } else {
            status = "NORMAL";
        }
        current_bpm = bpm; 
        strncpy((char*)current_status, status, sizeof(current_status)-1);

        // ONLY print every 15 samples
        if (++print_counter >= 15) {
            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("[Vitals] HR: %d BPM Status: %s\n", bpm, status);        
            xSemaphoreGive(print_mutex);
            print_counter = 0;
        }

        if (sample_count >= 3000) {
            avg_bpm_last_min = bpm_sum / 3000;
            bpm_sum = 0;
            sample_count = 0;
            printf("[REPORT] 1-Min Average: %d BPM\n", avg_bpm_last_min);
            //Reset peaks
            peak_bpm = 0; 
            low_bpm = 200;
        }


        if (bpm > 100 || bpm < 60) {
            if (!alert_active) {
                  alert_active = true;
              
                if(SEMCNT < MAX_COUNT_SEM+1) SEMCNT++; // DO NOT REMOVE THIS LINE
                    xSemaphoreGive(sem_sensor);  // Signal sensor event         

                    xSemaphoreTake(print_mutex, portMAX_DELAY);
                    printf(">>> CRITICAL EVENT: Patient entering %s state <<<\n", (bpm > 100) ? "Tachycardia" : "Bradycardia");
                    xSemaphoreGive(print_mutex);
                  }
        } else {
          alert_active = false;    
        }

        state_counter++;
        if (state_counter >= 15) {
            if (bpm > 100) tachycardia_seconds++;
            else if (bpm < 60) bradycardia_seconds++;
            state_counter = 0;
        }
        total_window_samples++;
        if (total_window_samples >= SAMPLES_PER_5_MIN) {
            // Reset statistics for a fresh 5-minute epoch
            peak_bpm = 0;
            low_bpm = 200;
            tachycardia_seconds = 0;
            bradycardia_seconds = 0;
            
            total_window_samples = 0;
            printf("[SYSTEM] 5-Minute Window Reset. Starting new epoch.\n");
        }
        gpio_set_level(DEBUG_PIN_SENSOR, 0);
        //printf("[Sensor] %lu ms\n", xTaskGetTickCount() * portTICK_PERIOD_MS);        gpio_set_level(DEBUG_PIN_SENSOR, 0);
        
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(20));;
    }
}
// TASK: button_task
// Company Role: Patient Assistance Call Handler
// Period: Event Driven + 10 ms debounce period
// Deadline: Soft Real-Time
// Priority: Highest (4)
// Purpose: Responds to nurse-call button requests.
void button_task(void *pvParameters) {
    while (1) {        
        if (xSemaphoreTake(sem_button, portMAX_DELAY)){
            nurse_call_active = true;                     
            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("[Patient] Emergency Nurse Call Button Pressed!\n");
            xSemaphoreGive(print_mutex);

            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
        }
        //printf("[Button] %lu ms Nurse request\n", xTaskGetTickCount() * portTICK_PERIOD_MS);        
        vTaskDelay(pdMS_TO_TICKS(10)); // Do Not Modify This Delay!
        while(xSemaphoreTake(sem_button, 0));
    }
}

// TASK: event_handler_task
// Company Role: Central Alarm / Response Controller
// Period: Reactive + 10 ms idle loop
// Deadline: HARD for sensor alerts
// Priority: Medium (2)
// Purpose: Handles out-of-range vitals alerts and
// nurse call acknowledgements.void event_handler_task(void *pvParameters) 
void event_handler_task(void *pvParameters){
    while (1) {
      
        if (xSemaphoreTake(sem_sensor, 0)) {
            SEMCNT--;  // DO NOT MODIFY THIS LINE

            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("\nSensor event: Vital signs out of range!\n");
            xSemaphoreGive(print_mutex);

            for (int i = 0; i < 5; i++) {

                gpio_set_level(Alert_LED, 1);
                vTaskDelay(pdMS_TO_TICKS(80));
                gpio_set_level(Alert_LED, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        

        if (xSemaphoreTake(sem_button, pdMS_TO_TICKS(10))) {
            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("\n[SYSTEM RESPONSE] Emergency Nurse Call acknowledged.\n");
            xSemaphoreGive(print_mutex);

            gpio_set_level(Alert_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(Alert_LED, 0);
        }
        //printf("[Event] %lu ms Alert handled\n", xTaskGetTickCount() * portTICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(10)); // Idle delay to yield CPU
    }
}

// ISR: button_isr_handler
// Company Role: Emergency Interrupt Trigger
// Deadline: HARD Immediate Response
// Purpose: Gives semaphore to wake nurse-call task.
static void IRAM_ATTR button_isr_handler(void* arg) {
    xSemaphoreGiveFromISR(sem_button, NULL);
}

void app_main()
{

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(POT_ADC_CHANNEL, ADC_ATTEN_DB_11);

    //esp_log_level_set( ESP_LOG_INFO);
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf( "ESP_WIFI_MODE_STA");
    connect_wifi();


    
    // GPIO initialization
    gpio_reset_pin(LED_GREEN);
    gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);

    gpio_reset_pin(Alert_LED);
    gpio_set_direction(Alert_LED, GPIO_MODE_OUTPUT);

    gpio_reset_pin(DEBUG_PIN_SENSOR);
    gpio_set_direction(DEBUG_PIN_SENSOR, GPIO_MODE_OUTPUT);

    // Configure Button Pin
    // 
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Wokwi buttons usually pull to GND
        .intr_type = GPIO_INTR_NEGEDGE   // Trigger on Falling Edge (Press)
    };
    gpio_config(&btn_conf);

    led_state = 0;
    printf( "LED Control Web Server is running ... ...\n");
    
    gpio_install_isr_service(0);
    gpio_set_intr_type(BUTTON_PIN, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);

    sem_button = xSemaphoreCreateBinary();
    sem_sensor = xSemaphoreCreateCounting(MAX_COUNT_SEM, 0);
    print_mutex = xSemaphoreCreateMutex();
    setup_server();
    

    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL,1);
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 3, NULL,1);
    xTaskCreatePinnedToCore(button_task, "button", 2048, NULL, 4, NULL,1);
    xTaskCreatePinnedToCore(event_handler_task, "event_handler", 4096, NULL, 2, NULL,1);

}