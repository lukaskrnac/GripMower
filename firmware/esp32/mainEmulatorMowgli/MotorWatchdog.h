#pragma once

#include <Arduino.h>

constexpr uint8_t MOTOR_POWER_PIN = 4; // HIGH pritiahne hlavny NO stykac

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

extern "C" void IRAM_ATTR esp_task_wdt_isr_user_handler(void) {
    // GPIO4 vypni uz v IRQ watchdogu, este pred resetom. Bez volania Arduino/HAL.
    REG_WRITE(GPIO_OUT_W1TC_REG, 1u << MOTOR_POWER_PIN);
}
#endif

inline bool startMotorWatchdog() {
#if defined(ARDUINO_ARCH_ESP32)
#if ESP_IDF_VERSION_MAJOR >= 5
    const esp_task_wdt_config_t config = {1000, 0, true}; // 1 s, reset pri zablokovanom loop()
    esp_err_t result = esp_task_wdt_init(&config);
    if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_reconfigure(&config);
#else
    const esp_err_t result = esp_task_wdt_init(1, true);
#endif
    return result == ESP_OK &&
           (esp_task_wdt_status(nullptr) == ESP_OK || esp_task_wdt_add(nullptr) == ESP_OK);
#else
    return true; // simulator na PC
#endif
}

inline bool feedMotorWatchdog() {
#if defined(ARDUINO_ARCH_ESP32)
    return esp_task_wdt_reset() == ESP_OK;
#else
    return true;
#endif
}

inline bool motorResetNeedsAck() {
#if defined(ARDUINO_ARCH_ESP32)
    const esp_reset_reason_t reason = esp_reset_reason();
    return reason != ESP_RST_POWERON && reason != ESP_RST_EXT;
#else
    return false;
#endif
}
