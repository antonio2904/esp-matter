/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <app_bridged_device.h>
#include <app_priv.h>
#include <app_espnow.h>
#include <app_reset.h>

static const char *TAG = "app_main";
uint16_t light_endpoint_id = 0;
uint16_t aggregator_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %d, effect: %d", type, effect_id);
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Driver update */
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

esp_err_t wifi_is_provisioned(bool *provisioned)
{
    if (!provisioned) {
        return ESP_ERR_INVALID_ARG;
    }

    *provisioned = false;

    /* Get Wi-Fi Station configuration */
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        return ESP_FAIL;
    }

    if (strlen((const char *) wifi_cfg.sta.ssid)) {
        *provisioned = true;
    }
    return ESP_OK;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    /* Initialize driver */
    app_driver_handle_t light_handle = app_driver_light_init();
    app_driver_handle_t button_handle = app_driver_button_init();
    app_reset_button_register(button_handle);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);

    /* This node handle can be used to create/add other endpoints and clusters. */
    if (!node) {
        ESP_LOGE(TAG, "Matter node creation failed");
    }

    color_temperature_light::config_t light_config;
    light_config.on_off.on_off = DEFAULT_POWER;
    light_config.on_off.lighting.start_up_on_off = nullptr;
    light_config.level_control.current_level = DEFAULT_BRIGHTNESS;
    light_config.level_control.lighting.start_up_current_level = DEFAULT_BRIGHTNESS;
    light_config.color_control.color_mode = EMBER_ZCL_COLOR_MODE_COLOR_TEMPERATURE;
    light_config.color_control.enhanced_color_mode = EMBER_ZCL_COLOR_MODE_COLOR_TEMPERATURE;
    light_config.color_control.color_temperature.startup_color_temperature_mireds = nullptr;
    endpoint_t *endpoint = color_temperature_light::create(node, &light_config, ENDPOINT_FLAG_NONE, light_handle);
    if (!endpoint) {
        ESP_LOGE(TAG, "Matter color temperature light endpoint creation failed");
    }

    endpoint_t *aggregator = endpoint::aggregator::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!aggregator) {
        ESP_LOGE(TAG, "Matter aggregator endpoint creation failed");
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);

    aggregator_endpoint_id = endpoint::get_id(aggregator);
    ESP_LOGI(TAG, "Switch created with endpoint id %d", aggregator_endpoint_id);

    /* Add additional features to the node */
    cluster_t *cluster = cluster::get(endpoint, ColorControl::Id);
    cluster::color_control::feature::hue_saturation::config_t hue_saturation_config;
    hue_saturation_config.current_hue = DEFAULT_HUE;
    hue_saturation_config.current_saturation = DEFAULT_SATURATION;
    cluster::color_control::feature::hue_saturation::add(cluster, &hue_saturation_config);

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter start failed: %d", err);
    }

    err = app_bridge_initialize(node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resume the bridged endpoints: %d", err);
    }

    bool provisioned = false;
    /* Let's find out if the device is provisioned previously */
    ESP_ERROR_CHECK(wifi_is_provisioned(&provisioned));
    if (provisioned) {
        ESP_LOGI(TAG, "WiFi already provisioned previously, disable PS");
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    app_espnow_init();

    /* Starting driver with default values */
    app_driver_light_set_defaults(light_endpoint_id);

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::init();
#endif
}
