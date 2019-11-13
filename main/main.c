#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#define LED_GPIO_OUTPUT_IO_0 23

//Define this variable to be used in logging macros eg ESP_LOGI(tag, format, ...)
static const char *TAG = "IOT_BOARD";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

// Define max string length for messages
// FIXME is this the right data type? also note that name of device takes away
// from number of available characters
const static uint8_t MAX_MSG_LEN = 140;

// Configure LED pin
static void led_init(void)
{
  // Configure the IOMUX register for pad LED_GPIO_OUTPUT_IO_0
  gpio_pad_select_gpio(LED_GPIO_OUTPUT_IO_0);
  // Set the GPIO as a push/pull output
  gpio_set_direction(LED_GPIO_OUTPUT_IO_0, GPIO_MODE_OUTPUT);
  // Start with the LED off
  gpio_set_level(LED_GPIO_OUTPUT_IO_0, 0);
}

// Process message
static void process_message(char *topic, size_t topic_len, char *msg, size_t msg_len)
{
  printf("topic: %s, msg: %s\n", topic, msg);
  if (strncasecmp(topic, "/board/led", topic_len) == 0)
  {
    if (strncasecmp(msg, "on", msg_len) == 0)
    {
        gpio_set_level(LED_GPIO_OUTPUT_IO_0, 1);
        ESP_LOGI(TAG, "Turning LED on.");
    } else if (strncasecmp(msg, "off", msg_len) == 0) {
        gpio_set_level(LED_GPIO_OUTPUT_IO_0, 0);
        ESP_LOGI(TAG, "Turning LED off");
    } else {
        ESP_LOGI(TAG, "Unable to match message");
    }
  } else {
    ESP_LOGI(TAG, "Unable to match topic");
  }
}

// Add device name to message string
static void add_device_name_to_msg(char *msg_dest, const char *msg)
{
  if((strlen(msg)+strlen(CONFIG_DEVICE_NAME)+5) > MAX_MSG_LEN){
    ESP_LOGE(TAG, "Message to be published too long. Aborting...");
    abort();
  } else {
    char data[(strlen(msg)+strlen(CONFIG_DEVICE_NAME)+5)];  // 5 chosen to allow for additional characters in message
    sprintf(data, "%s => %s", CONFIG_DEVICE_NAME, msg);
    strlcpy(msg_dest, data, sizeof(data));
  }
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  char data[MAX_MSG_LEN];

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
      add_device_name_to_msg(data, "Online");
      msg_id = esp_mqtt_client_publish(client, "/monitor/status", data, 0, 2, 1);
      ESP_LOGI(TAG, "Publish to /monitor/status successful, msg_id=%d", msg_id);

      msg_id = esp_mqtt_client_subscribe(client, "/board/led", 1);
      ESP_LOGI(TAG, "Subscribe to /board/led successful, msg_id=%d", msg_id);
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      break;

    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_PUBLISHED:
      ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
      printf("DATA=%.*s\r\n", event->data_len, event->data);
      process_message(event->topic, event->topic_len, event->data, event->data_len);
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
      break;
    default:
      ESP_LOGI(TAG, "Other event id:%d", event->event_id);
      break;
  }
  return ESP_OK;
}


static void mqtt_start(void)
{
  char lwt_data[MAX_MSG_LEN];
  add_device_name_to_msg(lwt_data, "Offline");
  esp_mqtt_client_config_t mqtt_cfg = {
    .uri = CONFIG_BROKER_URL,
    .event_handle = mqtt_event_handler,
    .lwt_topic = "/monitor/status",
    .lwt_msg = lwt_data,
    .lwt_qos = 2,
    .lwt_retain = 1,
  };

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_start(client);
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
  switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
      esp_wifi_connect();
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      esp_wifi_connect();
      xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
      break;
    default:
      break;
  }
  return ESP_OK;
}

static void wifi_init(void)
{
  tcpip_adapter_init();
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi_config = {
    .sta = {
      .ssid = CONFIG_WIFI_SSID,
      .password = CONFIG_WIFI_PASSWORD,
    },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "Waiting for wifi");
  xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

void app_main(void)
{
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
  esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

  nvs_flash_init();
  wifi_init();

  led_init();

  mqtt_start();
}
