#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "dht11.h"

#define WIFI_SSID "Marcelo" // SSID da rede Local
#define WIFI_PASS "marcelo123" // Senha da Rede Local
#define MQTT_BROKER_IP "test.mosquitto.org" // IP do broker MQTT local
#define MQTT_BROKER_PORT 1883 // Porta escolhida para o BROKER
#define MQTT_TOPIC_TEMP "mestrado/iot/aluno/marcelo/temperatura" // Tópico para Temperatura
#define MQTT_TOPIC_HUMID "mestrado/iot/aluno/marcelo/umidade" // Tópico para Humidade
#define DHT_PIN GPIO_NUM_0  // D3 corresponde ao GPIO 0 no ESP8266

static const char *TAG = "DHT11_MQTT";

// Declarando o Event Group para gerenciar a conexão Wi-Fi
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static esp_mqtt_client_handle_t mqtt_client;

// Função de manipulação de eventos para Wi-Fi
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 10) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Tentando reconectar ao AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Falha na conexão ao AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado, IP obtido: %s", ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Função de manipulação de eventos para MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    mqtt_client = event->client;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao broker MQTT.");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Desconectado do broker MQTT.");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Mensagem publicada com sucesso.");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "Erro no evento MQTT.");
            break;
        default:
            ESP_LOGI(TAG, "Evento MQTT não tratado, id:%d", event->event_id);
            break;
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

void mqtt_app_start(void) {
    char mqtt_uri[50];
    snprintf(mqtt_uri, sizeof(mqtt_uri), "mqtt://%s:%d", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    esp_mqtt_client_config_t mqtt_cfg = { .uri = mqtt_uri };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void read_dht11_and_publish_task(void *pvParameters) {
    DHT11_init(DHT_PIN);

    while (1) {
        struct dht11_reading dht_data = DHT11_read();

        if (dht_data.status == DHT11_OK) {
            // Envia temperatura para o tópico específico
            char temp_payload[20];
            snprintf(temp_payload, sizeof(temp_payload), "%d", dht_data.temperature);
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_TEMP, temp_payload, 0, 1, 0);
            ESP_LOGI(TAG, "Temperatura enviada: %s", temp_payload);

            // Envia umidade para o tópico específico
            char humid_payload[20];
            snprintf(humid_payload, sizeof(humid_payload), "%d", dht_data.humidity);
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_HUMID, humid_payload, 0, 1, 0);
            ESP_LOGI(TAG, "Umidade enviada: %s", humid_payload);

        } else {
            ESP_LOGE(TAG, "Erro ao ler dados do DHT11");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));  // Envia dados a cada 10 segundos
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    // Configura o MAC Address manualmente
    uint8_t base_mac[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
    esp_base_mac_addr_set(base_mac);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    mqtt_app_start();

    // Cria a tarefa de leitura e publicação
    xTaskCreate(read_dht11_and_publish_task, "read_dht11_and_publish_task", 2048, NULL, 5, NULL);
}
