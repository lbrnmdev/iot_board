#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <time.h>
#include <sys/time.h>

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

// adc includes
#include "driver/adc.h"
#include "esp_adc_cal.h"

// sleep & rtc includes
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/rtc.h"

#define LED_GPIO_OUTPUT_IO_0 23       // GPIO 23 for led output
#define BUTTON_GPIO_INPUT_IO_0 4
#define GPIO_INPUT_PIN_SEL ((1ULL<<BUTTON_GPIO_INPUT_IO_0)) // create bit mask of pin
#define ESP_INTR_FLAG_DEFAULT 0

// adc defines
#define DEFAULT_VREF          1100    // Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES         64      // for multisampling

//Define this variable to be used in logging macros eg ESP_LOGI(tag, format, ...)
static const char *TAG = "IOT_BOARD";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

static EventGroupHandle_t mqtt_event_group;
const static int MQTT_HEALTH_BIT = BIT0;

static RTC_DATA_ATTR struct timeval sleep_enter_time;

// global mqtt client
esp_mqtt_client_handle_t global_mqtt_client;

// Queue for gpio events
static xQueueHandle gpio_evt_queue = NULL;

// Define max string length for messages
// FIXME is this the right data type? also note that name of device takes away
// from number of available characters
const static uint8_t MAX_MSG_LEN = 140;

// adc variables
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel_tmp36 = ADC_CHANNEL_7;     // GPIO35 if ADC1, GPIO14 if ADC2
static const adc_atten_t atten = ADC_ATTEN_DB_0;              // attenuation of 0dB giving full-scale voltage of 1.1V
static const adc_unit_t unit = ADC_UNIT_1;                    // use ADC1

// temp value
static char tmp36_temp_str[140];

// determine supported calibration values
static void check_efuse()
{
  // check TP is burned into eFuse
  if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
    ESP_LOGI(TAG, "eFuse Two Point: Supported\n");
  } else {
    ESP_LOGI(TAG, "eFuse Two Point: NOT supported\n");
  }

  // check Vref is burned into eFuse
  if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
    ESP_LOGI(TAG, "eFuse Vref: Supported\n");
  } else {
    ESP_LOGI(TAG, "eFuse Vref: NOT supported\n");
  }
}

// log type of characterization used
static void print_char_val_type(esp_adc_cal_value_t val_type)
{
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
    ESP_LOGI(TAG, "Characterized using Two Point Value\n");
  } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    ESP_LOGI(TAG, "Characterized using eFuse Vref\n");
  } else {
    ESP_LOGI(TAG, "Characterized using Default Vref\n");
  }
}

static void config_adc()
{
  // config adc width and channel attenuation
  if (unit == ADC_UNIT_1) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(channel_tmp36, atten);
  } else {
    adc2_config_channel_atten((adc2_channel_t)channel_tmp36, atten);
  }

  // characterize adc
  adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, ADC_WIDTH_BIT_12, DEFAULT_VREF, adc_chars);
  print_char_val_type(val_type);
}

// obtain raw adc reading
static uint32_t read_adc()
{
  uint32_t reading = 0;
  // multisample
  for (int i = 0; i < NO_OF_SAMPLES; i++) {
    if (unit == ADC_UNIT_1) {
      reading += adc1_get_raw((adc1_channel_t)channel_tmp36);
    } else {
      int raw;
      adc2_get_raw((adc2_channel_t)channel_tmp36, ADC_WIDTH_BIT_12, &raw);
      reading += raw;
    }
  }
  reading /= NO_OF_SAMPLES;
  return reading;
}

// Configure LED pin
static void led_init(void)
{
  // Configure the IOMUX register for pad LED_GPIO_OUTPUT_IO_0
  gpio_pad_select_gpio(LED_GPIO_OUTPUT_IO_0);
  // Set the GPIO as a push/pull output
  gpio_set_direction(LED_GPIO_OUTPUT_IO_0, GPIO_MODE_OUTPUT);
  // Start with the LED on. This way user knows when device is awake
  gpio_set_level(LED_GPIO_OUTPUT_IO_0, 1);
}

// Process message
static void process_message(char *topic, size_t topic_len, char *msg, size_t msg_len)
{
  printf("topic: %.*s, msg: %.*s\n", topic_len, topic, msg_len, msg);
  // TODO: Currently no real point in toggling LED via mqtt.
  // TODO: Handle case where topic is /board/sleep/time.
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
  // TODO: Include timestamp in all published data

  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  char data[MAX_MSG_LEN];

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
      add_device_name_to_msg(data, "Online"); // TODO: wakeup source data
      msg_id = esp_mqtt_client_publish(client, "/monitor/status", data, 0, 2, 1);
      ESP_LOGI(TAG, "Publish to /monitor/status successful, msg_id=%d", msg_id);

      msg_id = esp_mqtt_client_subscribe(client, "/board/led", 1);
      ESP_LOGI(TAG, "Subscribe to /board/led successful, msg_id=%d", msg_id);

      add_device_name_to_msg(data, tmp36_temp_str);
      msg_id = esp_mqtt_client_publish(client, "/board/temp", data, 0, 1, 0);
      ESP_LOGI(TAG, "Publish to /board/temp successful, msg_id=%d, temp in celsius", msg_id);
      xEventGroupSetBits(mqtt_event_group, MQTT_HEALTH_BIT);
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      xEventGroupClearBits(mqtt_event_group, MQTT_HEALTH_BIT);
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
  add_device_name_to_msg(lwt_data, "MQTT timeout/disconnect");
  esp_mqtt_client_config_t mqtt_cfg = {
    .uri = CONFIG_BROKER_URL,
    .event_handle = mqtt_event_handler,
    .lwt_topic = "/monitor/status",
    .lwt_msg = lwt_data,
    .lwt_qos = 2,
    .lwt_retain = 1,
  };

  mqtt_event_group = xEventGroupCreate();

  global_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_start(global_mqtt_client);

  EventBits_t uxBitsToWaitFor;
  const TickType_t xTicksToWaitFor = 60000 / portTICK_PERIOD_MS; // 60 sec timeout
  uxBitsToWaitFor = xEventGroupWaitBits(mqtt_event_group, MQTT_HEALTH_BIT, false, true, xTicksToWaitFor);
  if ( ( uxBitsToWaitFor & MQTT_HEALTH_BIT ) != 0 )
  {
    printf("MQTT health success\n");    // successful
  }
  else {
    printf("MQTT health fail, timeout\n");   // timeout
  }
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

/**
* TODO: Have this version of wifi_init return EventBits_t which can be tested to
* if the connection attempt has been successful or not. Or return boolean?
*/
static bool wifi_init(void)
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

  EventBits_t uxBits;
  const TickType_t xTicksToWait = 30000 / portTICK_PERIOD_MS; // 30 sec timeout
  uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, xTicksToWait);

  if ( ( uxBits & CONNECTED_BIT ) != 0 )
  {
    return true;    // successful connection
  }
  else {
    return false;   // timeout
  }
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
  uint32_t gpio_num = (uint32_t) arg;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void button_to_mqtt_task(void* arg)
{
  // FIXME: declaring + initializing too many variables.
  // TODO: Currently device is on for too short a period for button task to
  //        work properly, so create timer that resets on each button press and
  //        deep sleeps at timer's end.
  uint32_t io_num, button_level;
  int msg_id;
  char data[MAX_MSG_LEN];
  for(;;) {
    if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      button_level = gpio_get_level(io_num);
      ESP_LOGI(TAG, "GPIO[%d] intr, val: [%d]", io_num, button_level);
      add_device_name_to_msg(data, "Button press"); // TODO modify to include which button
      msg_id = esp_mqtt_client_publish(global_mqtt_client, "/board/button", data, 0, 1, 0);
      ESP_LOGI(TAG, "Publish to /board/button successful, msg_id=%d", msg_id);
    }
  }
}

static void button_init(void)
{
  // deinit button as rtc gpio, as waking from deep sleep leaves it in rtc mode
  // and it doesn't function properly
  rtc_gpio_deinit(BUTTON_GPIO_INPUT_IO_0);
  // config struct
  gpio_config_t io_conf;
  // interrupt of falling edge
  io_conf.intr_type = GPIO_INTR_NEGEDGE;
  // set bit mask of the pins
  io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
  // set to input mode
  io_conf.mode = GPIO_MODE_INPUT;
  // disable pull-down mode
  io_conf.pull_down_en = 0;
  // enable pull-up mode
  io_conf.pull_up_en = 1;
  // configure gpio
  gpio_config(&io_conf);
}

static void isr_init(void)
{
  // create a queue for gpio events
  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  // start gpio task
  xTaskCreate(button_to_mqtt_task, "button_to_mqtt_task", 2048, NULL, 10, NULL);

  // install gpio isr service
  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
  // hook isr handler for specific gpio pin
  gpio_isr_handler_add(BUTTON_GPIO_INPUT_IO_0, gpio_isr_handler, (void*) BUTTON_GPIO_INPUT_IO_0);
}

void app_main(void)
{
  struct timeval now;
  gettimeofday(&now, NULL);
  int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

  // TODO: incorporate this into the switch case below.
  // wakeup source flag
  // 0 => timer; 1 => gpio
  int source = 0;
  // wakeup info char array
  char wakeup_data[MAX_MSG_LEN];

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      source = 1;
      uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_pin_mask != 0) {
        int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
        printf("Wake up from GPIO %d\n", pin);
      } else {
        printf("Wake up from GPIO\n");
      }
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER: {
      ESP_LOGI(TAG, "Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
      break;
    }
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      ESP_LOGI(TAG, "Not a deep sleep reset\n");
  }

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

  led_init();
  button_init();

  // take temp reading
  check_efuse();
  config_adc();
  uint32_t tmp36_reading = read_adc();
  printf("raw adc in app_main(): %d\n", tmp36_reading);
  uint32_t tmp36_voltage = esp_adc_cal_raw_to_voltage(tmp36_reading, adc_chars);
  printf("raw voltage in app_main(): %dmV\n", tmp36_voltage);
  uint32_t tmp36_temp = (tmp36_voltage - 500)/10;
  printf("raw temp in app_main(): %d celsius\n", tmp36_temp);
  sprintf(tmp36_temp_str, "Temp: %d Celsius", tmp36_temp);

  if( wifi_init() )
  {
    isr_init();
    mqtt_start();

    // TODO: clean this up, maybe move this logic to switch statement above or
    // better yet mqtt event handler
    // TODO: include length of sleep time, also time of day
    int wakeup_msg_id;
    if (source == 1){
      add_device_name_to_msg(wakeup_data, "Wakeup from sleep. Source: GPIO");
      wakeup_msg_id = esp_mqtt_client_publish(global_mqtt_client, "/monitor/status", wakeup_data, 0, 2, 1);
      ESP_LOGI(TAG, "Publish to /monitor/status successful, wakeup status, msg_id=%d", wakeup_msg_id);
    } else {
      add_device_name_to_msg(wakeup_data, "Wakeup from sleep. Source: Timer");
      wakeup_msg_id = esp_mqtt_client_publish(global_mqtt_client, "/monitor/status", wakeup_data, 0, 2, 1);
      ESP_LOGI(TAG, "Publish to /monitor/status successful, wakeup status, msg_id=%d", wakeup_msg_id);
    }

    esp_mqtt_client_stop(global_mqtt_client);
  } else
  {
    printf("Unable to connect to WiFi\n");
  }

  ESP_ERROR_CHECK(esp_wifi_stop());
  ESP_LOGI(TAG, "Stopping wifi");

  // TODO: is it possible to debounce button press?
  printf("Enabling EXT1 wakeup on pin GPIO%d\n", BUTTON_GPIO_INPUT_IO_0);
  esp_sleep_enable_ext1_wakeup(GPIO_INPUT_PIN_SEL, ESP_EXT1_WAKEUP_ANY_HIGH);

  printf("Entering deep sleep\n");
  gettimeofday(&sleep_enter_time, NULL);
  // TODO: Allow sleep time to be sent via mqtt
  esp_deep_sleep(60000000);   // deep sleep for 60 seconds
}
