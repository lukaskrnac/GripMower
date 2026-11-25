#include <Arduino.h>
#include <FastCRC.h>
#include <PacketSerial.h>
#include "Mm32Motors.h"
#include "MotorWatchdog.h"
#include "imu.h"
#include "BTS7960.h"
#include "AS5600Wire1.h"  // pozor upravena v AS5600.h na Wire1

AS5600 as5600;

// ============================================================================
//  PINY
// ============================================================================
#define EMERGENCY_PIN 13
#define DOCK_PIN 34

// ============================================================================
//  ID PAKETOV
// ============================================================================0
#define PACKET_ID_LL_STATUS                 0x01  ///< STM32 -> Pi: system status
#define PACKET_ID_LL_IMU                    0x02  ///< STM32 -> Pi: IMU data
#define PACKET_ID_LL_UI_EVENT               0x03  ///< STM32 -> Pi: UI button event
#define PACKET_ID_LL_ODOMETRY               0x04  ///< STM32 -> Pi: wheel odometry
#define PACKET_ID_LL_BLADE_STATUS           0x05  ///< STM32 -> Pi: blade motor status
#define PACKET_ID_LL_HIGH_LEVEL_CONFIG_REQ  0x11  ///< Bidirectional: config request
#define PACKET_ID_LL_HIGH_LEVEL_CONFIG_RSP  0x12  ///< Bidirectional: config response
#define PACKET_ID_LL_HEARTBEAT              0x42  ///< Pi -> STM32: heartbeat
#define PACKET_ID_LL_HIGH_LEVEL_STATE       0x43  ///< Pi -> STM32: high-level state
#define PACKET_ID_LL_CMD_VEL                0x50  ///< Pi -> STM32: velocity command (extension)
#define PACKET_ID_LL_CMD_BLADE              0x51  ///< Pi -> STM32: blade motor control
#define PACKET_ID_LL_REBOOT                 0x52  ///< Pi -> STM32: reboot the board (NVIC_SystemReset)

// MowgliNext v1.6.3 / aktualny main: wire protocol v9.
// Vlastny ESP32 firmware, verzia 0.9.3; nie originalny STM32 firmware.
#define MOWGLI_PROTOCOL_VERSION 9u
#define PACKET_ID_LL_PARAM_VALUE        0x13
#define PACKET_ID_LL_PARAM_STORE_STATUS 0x14
#define PACKET_ID_LL_SET_PARAM          0x58
#define PACKET_ID_LL_GET_PARAM          0x59
#define PACKET_ID_LL_PARAM_COMMIT       0x5A
#define PACKET_ID_LL_PARAM_STORE_RESET  0x5B

#define WHEEL_DIST 0.52  // vzdialenost medzi kolasema
#define STEER_DIST 0.52   // vzdialenost osi od predneho kolesa
#define WHEEL_HOLD_BRAKE 1 // 1 = kolesa zostavaju v RUN na 0 ot./s (aktivna brzda), 0 = IDLE v stoji

enum HighLevelMode {
    MODE_IDLE = 1,       // ROS connected, idle mode
    MODE_AUTONOMOUS = 2, // ROS connected, Autonomous mode, either mowing or docking or undocking
    MODE_RECORDING = 3   // ROS connected, Manual mode during recording etc
};

// ============================================================================
//  EMERGENCY BITY
// ============================================================================
#define LL_EMERGENCY_BIT_LATCH 0b00000001
#define LL_EMERGENCY_BIT_HALL1 0b00001000 // Lift1
#define LL_EMERGENCY_BIT_HALL2 0b00010000 // Lift2
#define LL_EMERGENCY_BIT_HALL3 0b00000010 // Stop1
#define LL_EMERGENCY_BIT_HALL4 0b00000100 // Stop2

#define LL_EMERGENCY_BIT_LIFT1 LL_EMERGENCY_BIT_HALL1
#define LL_EMERGENCY_BIT_LIFT2 LL_EMERGENCY_BIT_HALL2
#define LL_EMERGENCY_BITS_LIFT (LL_EMERGENCY_BIT_LIFT1 | LL_EMERGENCY_BIT_LIFT2)
#define LL_EMERGENCY_BIT_STOP1 LL_EMERGENCY_BIT_HALL3
#define LL_EMERGENCY_BIT_STOP2 LL_EMERGENCY_BIT_HALL4
#define LL_EMERGENCY_BITS_STOP (LL_EMERGENCY_BIT_STOP1 | LL_EMERGENCY_BIT_STOP2)

#define LL_STATUS_BIT_UI_AVAIL 0b10000000

// ============================================================================
//  ŠTRUKTÚRY PAKETOV
// ============================================================================
#pragma pack(push, 1)
struct ll_status {
    // Type of this message. Has to be PACKET_ID_LL_STATUS.
    uint8_t type;
    // Bitmask for rain, sound, powers etc
    // Bit 0: Initialized (i.e. setup() was a success). If this is 0, all other bits are meaningless.
    // Bit 1: Raspberry Power
    // Bit 2: Charging enabled
    // Bit 3: don't care
    // Bit 4: Rain detected
    // Bit 5: Sound available
    // Bit 6: Sound busy
    // Bit 7: UI Board available
    uint8_t status_bitmask;
    // USS range in m
    float uss_ranges_m[5];
    // Emergency bitmask:
    // Bit 0: Emergency latch
    // Bit 1: Emergency/Hall 3 (Stop1) active
    // Bit 2: Emergency/Hall 4 (Stop2) active
    // Bit 3: Emergency/Hall 1 (Lift1) active
    // Bit 4: Emergency/Hall 2 (Lift2) active
    uint8_t emergency_bitmask;
    // Charge voltage
    float v_charge;
    // System voltage
    float v_battery;
    // Charge current
    float charging_current;
    uint8_t batt_percentage;
    uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_imu {
    // Type of this message. Has to be PACKET_ID_LL_IMU.
    uint8_t type;
    // Time since last message in milliseconds.
    uint16_t dt_millis;
    // Acceleration[m^s2], Gyro[rad/s] and magnetic field[uT]
    float acceleration_mss[3];
    float gyro_rads[3];
    float mag_uT[3];
    uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_heartbeat {
    // Type of this message. Has to be PACKET_ID_LL_HEARTBEAT.
    uint8_t type;
    // True, if emergency should be engaged (e.g. navigation error, ...)
    // False to not change the latch
    uint8_t emergency_requested;
    // True, if emergency condition should be reset
    // False to not change the latch
    uint8_t emergency_release_requested;
    uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_high_level_state {
    // Type of this message. Has to be PACKET_ID_LL_HIGH_LEVEL_STATE
    uint8_t type;
    uint8_t current_mode; // see HighLevelMode
    uint8_t gps_quality;  // GPS quality in percent (0-100)
    uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_ui_event {
    // Type of this message. Has to be PACKET_ID_LL_UI_EVENT
    uint8_t type;
    uint8_t button_id;
    uint8_t press_duration; // 0 for single press, 1 for long, 2 for very long press
    uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_odometry {
    uint8_t type;               ///< Must equal PACKET_ID_LL_ODOMETRY
    uint32_t acquisition_millis; ///< Cas odometrickej snimky od startu ESP32 [ms]
    int32_t left_ticks;         ///< Signed cumulative left encoder ticks
    int32_t right_ticks;        ///< Signed cumulative right encoder ticks
    int16_t left_velocity_mm_s; ///< Signed left wheel velocity [mm/s]
    int16_t right_velocity_mm_s;///< Signed right wheel velocity [mm/s]
    uint16_t crc;               ///< CRC-16 CCITT over all preceding bytes
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_cmd_vel {
    uint8_t type;    ///< Must equal PACKET_ID_LL_CMD_VEL
    float linear_x;  ///< Forward velocity [m/s]
    float angular_z; ///< Angular (yaw) velocity [rad/s]
    uint16_t crc;    ///< CRC-16 CCITT over all preceding bytes
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_cmd_blade {
    uint8_t type;      ///< Must equal PACKET_ID_LL_CMD_BLADE
    uint8_t blade_on;  ///< 1=start, 0=stop
    uint8_t blade_dir; ///< 0=normal, 1=reverse
    uint16_t crc;      ///< CRC-16 CCITT over all preceding bytes
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
struct ll_blade_status {
    uint8_t type;        ///< Must equal PACKET_ID_LL_BLADE_STATUS
    uint8_t is_active;   ///< 1=running, 0=stopped
    uint16_t rpm;        ///< Blade motor RPM
    uint16_t current_ma; ///< Súčet DC prúdov čepelí [mA], upstream názov power_watts
    float temperature;   ///< Blade/motor temperature [C]
    uint32_t error_count;///< Cumulative error counter
    uint16_t crc;        ///< CRC-16 CCITT over all preceding bytes
} __attribute__((packed));
#pragma pack(pop)

// Vsetky velkosti nizsie zahrnaju posledne 2 bajty CRC.
#pragma pack(push, 1)
struct ll_config_req {
    uint8_t type;
    uint8_t flags;
    uint16_t crc;
};
struct ll_config_rsp {
    uint8_t type;
    uint8_t protocol_version;
    uint8_t active_flags;
    uint8_t fw_version_major;
    uint8_t fw_version_minor;
    uint8_t fw_version_patch;
    uint16_t crc;
};
struct ll_param_value {
    uint8_t type;
    uint16_t param_id;
    uint8_t status;
    uint8_t flags;
    float value;
    float default_value;
    float min_value;
    float max_value;
    uint16_t crc;
};
struct ll_param_store_status {
    uint8_t type;
    uint8_t boot_source;
    uint8_t last_commit;
    uint16_t records_left;
    uint16_t param_count;
    uint32_t reset_request_id;
    uint16_t crc;
};
#pragma pack(pop)

static_assert(sizeof(ll_status) == 38, "ll_status: wire size");
static_assert(sizeof(ll_imu) == 41, "ll_imu: wire size");
static_assert(sizeof(ll_blade_status) == 16, "ll_blade_status: wire size");
static_assert(sizeof(ll_odometry) == 19, "ll_odometry v9: wire size");
static_assert(sizeof(ll_config_req) == 4, "ll_config_req: wire size");
static_assert(sizeof(ll_config_rsp) == 8, "ll_config_rsp: wire size");
static_assert(sizeof(ll_param_value) == 23, "ll_param_value: wire size");
static_assert(sizeof(ll_param_store_status) == 13, "ll_param_store_status: wire size");

#define LL_HIGH_LEVEL_CONFIG_MAX_COMMS_VERSION 1          // Max. comms packet version supported by this open_mower LL FW
#define LL_HIGH_LEVEL_CONFIG_BIT_DFPIS5V (1 << 0)         // Enable full sound via mower_config env var "OM_DFP_IS_5V"
#define LL_HIGH_LEVEL_CONFIG_BIT_EMERGENCY_INVERSE (1 << 1) // Sample, for possible future usage, i.e. for SA-Type emergency

BTS7960 servoController(25, 26, 27);

// ============================================================================
//  ČASOVANIA
// ============================================================================
#define IMU_CYCLETIME 20               // cycletime for refresh IMU data
#define STATUS_CYCLETIME 20            // cycletime for refresh analog and digital Statusvalues
#define UI_GET_VERSION_CYCLETIME 5000  // cycletime for UI Get_Version request (UI available check)
#define UI_GET_VERSION_TIMEOUT 100     // timeout for UI Get_Version response (UI available check)

#define TILT_EMERGENCY_MILLIS 2500 // Time for a single wheel to be lifted in order to count as emergency (0 disable). This is to filter uneven ground.
#define LIFT_EMERGENCY_MILLIS 100  // Time for both wheels to be lifted in order to count as emergency (0 disable). This is to filter uneven ground.
#define BUTTON_EMERGENCY_MILLIS 20 // Time for button emergency to activate. This is to debounce the button.
#define MOTOR_SEND_MILLIS 40
#define DRIVE_COMMAND_TIMEOUT 300
#define MOTOR_POWER_BOOT_TIMEOUT 2500
#define MOTOR_POWER_REARM_MILLIS 1000
#define BLADE_STATUS_MILLIS 125

// Define to stream debugging messages via USB
// #define USB_DEBUG

#define ANZ_SOUND_SD_FILES 3

// Millis after charging is retried
#define CHARGING_RETRY_MILLIS 10000

/**
 * @brief Some hardware parameters
 */
#define VIN_R1 10000.0f
#define VIN_R2 1000.0f
#define R_SHUNT 0.003f
#define CURRENT_SENSE_GAIN 100.0f

#define BATT_ABS_MAX 28.7f
#define BATT_ABS_Min 21.7f

#define BATT_FULL (BATT_ABS_MAX - 0.3f)
#define BATT_EMPTY (BATT_ABS_Min + 0.3f)

// Emergency will be engaged, if no heartbeat was received in this time frame.
#define HEARTBEAT_MILLIS 500


// ============================================================================
//  BEZPEČNOSTNÉ ZASTAVENIE PRI NAKLONENÍ
// ============================================================================
#define PITCH_STOP_DEG     35.0f
#define PITCH_REVERSE_DEG  35.0f
#define ROLL_STOP_DEG      45.0f
#define TILT_REVERSE_MILLIS 1000
#define TILT_REVERSE_MPS    -0.3f

bool tilt_latch = false;       // raz nastavené, zostáva zastavené až do manuálneho resetu
bool tilt_reversing = false;   // prebieha jednorazové cúvanie
unsigned long tilt_reverse_start_millis = 0;
// ============================================================================
//  KOMUNIKÁCIA
// ============================================================================
PacketSerial packetSerial; // COBS communication PICO <> Raspi
FastCRC16 CRC16;

#ifdef ENABLE_SOUND_MODULE
MP3Sound my_sound; // Soundsystem
#endif

// ============================================================================
//  ČASOVÉ ZNAČKY (millis)
// ============================================================================
unsigned long last_imu_millis = 0;
unsigned long last_status_update_millis = 0;
unsigned long last_heartbeat_millis = 0;
unsigned long next_ui_msg_millis = 0;
unsigned long last_servo_millis = 0;
uint32_t last_motor_send_millis = 0;
uint32_t last_drive_command_millis = 0;
uint32_t drive_arm_millis = 0, blade_arm_millis = 0, blade_stop_millis = 0;

unsigned long lift_emergency_started = 0;
unsigned long tilt_emergency_started = 0;
unsigned long button_emergency_started = 0;

unsigned long ui_get_version_next_millis = 0;     // Next cycle when to check for a UI version
unsigned long ui_get_version_respond_timeout = 0; // When UI Get_Version response times out

// ============================================================================
//  STAVOVÉ PREMENNÉ
// ============================================================================
// Stock UI
uint8_t stock_ui_emergency_state = 0; // Get set by received Get_Emergency packet
bool stock_ui_rain = false;           // Get set by received Get_Rain packet

bool emergency_latch = true;
bool ros_emergency = false;
bool motor_power_on = false, motor_power_ready = false;
bool motor_power_fault = false, stop_was_pressed = false;
bool motor_watchdog_ready = false;
void updateEmergency();
uint32_t motor_power_changed = 0, motor_error_count = 0;
uint32_t last_blade_status_millis = 0;
bool drive_neutral_seen = false;
bool sound_available = false;
bool charging_allowed = false;
bool ROS_running = false;
unsigned long charging_disabled_time = 0;

// Riadenie servo (natáčanie kolies)
float reqAngle = 0;
float actAngle = 0;
float actAngleOld = 0;
float actSpeed = 0;
float auxVar = 0;
int act_mode = 0, last_mode = 0;

// Čepeľ (blade)
bool blade_active = false;
uint8_t blade_direction = 0;
bool drive_armed = false, blades_armed = false;
bool drive_command_valid = false;
float left_speed_m_s = 0, right_speed_m_s = 0;

float imu_temp[9];

uint16_t ui_version = 0;    // Last received UI firmware version
uint16_t ui_interval = 1000;// UI send msg (LED/State) interval (ms)

// Some vars related to PACKET_ID_LL_HIGH_LEVEL_CONFIG_*
uint8_t comms_version = 0;  // comms packet version (>0 if implemented)
uint8_t config_bitmask = 0; // See LL_HIGH_LEVEL_CONFIG_BIT_*

// Predefined message buffers, so that we don't need to allocate new ones later.
struct ll_imu imu_message = {0};
struct ll_status status_message = {0};
struct ll_odometry odometry_message = {0};
// current high level state
struct ll_high_level_state last_high_level_state = {0};

// ============================================================================
//  DEKLARÁCIE FUNKCIÍ
// ============================================================================
void sendMessage(void *message, size_t size);
void sendUIMessage(void *message, size_t size);
void onPacketReceived(const uint8_t *buffer, size_t size);
bool crcOk(const uint8_t *buffer, size_t size);
void setBladeActive(bool active);
void stopDriveMotors();
void sendUnsupportedParam(uint16_t id);
void sendParamStoreStatus();
uint8_t param_store_last_result = 0; // PARAM_COMMIT_NONE; ziaden zapis do flash.

// ============================================================================
//  POMOCNÉ FUNKCIE
// ============================================================================

/**
 * Overí CRC-16/CCITT posledných 2 bajtov paketu oproti zvyšku dát.
 * Vracia true, ak je paket v poriadku a dá sa spracovať.
 */
bool crcOk(const uint8_t *buffer, size_t size) {
    if (size < 4) {
        return false;
    }
    uint16_t crc = CRC16.ccitt(buffer, size - 2);
    return buffer[size - 1] == ((crc >> 8) & 0xFF) &&
           buffer[size - 2] == (crc & 0xFF);
}

// Tento minimalny port nema runtime parametre ani parameter-flash originalnej
// STM32 dosky. Nikdy nepotvrdzuje neuskutocnene nastavenie/zapis ako uspesne.
void sendUnsupportedParam(uint16_t id) {
    ll_param_value response = {};
    response.type = PACKET_ID_LL_PARAM_VALUE;
    response.param_id = id;
    response.status = 2; // FW_PARAM_STATUS_UNKNOWN_ID
    sendMessage(&response, sizeof(response));
}

void sendParamStoreStatus() {
    ll_param_store_status response = {};
    response.type = PACKET_ID_LL_PARAM_STORE_STATUS;
    response.last_commit = param_store_last_result;
    // boot_source=DEFAULTS, param_count=0, records_left=0, reset_request_id=0.
    sendMessage(&response, sizeof(response));
}

void setRaspiPower(bool power) {
    // Update status bits in the status message
    status_message.status_bitmask = (status_message.status_bitmask & 0b11111101) | ((power & 0b1) << 1);
}

void setBladeActive(bool active) {
    blade_active = active;
    if (!active) {
        if (blades_armed) {
            mm32Idle(bladeBus);
            blade_stop_millis = millis();
        }
        blades_armed = false;
    }
}

void stopDriveMotors() {
    drive_command_valid = false;
    left_speed_m_s = right_speed_m_s = 0;
    if (drive_armed) mm32Idle(wheelBus);
    drive_armed = false;
}

void cutMotorPower() {
    // Kontaktor vypni skor, ako by mohla zablokovat seriova komunikacia.
    digitalWrite(MOTOR_POWER_PIN, LOW);
    if (!motor_power_on) return;
    motor_power_on = motor_power_ready = false;
    motor_power_changed = millis();
    drive_armed = blades_armed = false;
    drive_neutral_seen = false;
    stopDriveMotors();
    setBladeActive(false);
    tilt_reversing = false;
    servoController.TurnLeft(0);
    servoController.TurnRight(0);
}

void tripMotorPower() {
    if (!motor_power_fault) ++motor_error_count;
    motor_power_fault = true;
    cutMotorPower();
    updateEmergency();
}

void startMotorPower(uint32_t now) {
    // Kontrolery su po zapnuti nove. Predoslu telemetriu nepouzivaj na povolenie pohybu.
    for (Mm32Motor &motor : wheels) {
        const uint32_t ticks = motor.ticks;
        motor = Mm32Motor(motor.id, motor.direction);
        motor.ticks = ticks;
    }
    for (Mm32Motor &motor : blades) motor = Mm32Motor(motor.id, motor.direction);
    wheelBus.clearInput();
    bladeBus.clearInput();
    mm32BatteryVoltage = 0;
    motor_power_changed = now;
    motor_power_on = true;
    digitalWrite(MOTOR_POWER_PIN, HIGH);
}

void updateEmergency() {
    const uint32_t now = millis();
    const bool stopPressed = !digitalRead(EMERGENCY_PIN);
    if (stopPressed) {
        stop_was_pressed = true;
    } else if (stop_was_pressed) {
        // Po poruche je reset iba fyzickou stopkou. Obnovenie UARTu samo nic nezapne.
        stop_was_pressed = false;
        motor_power_fault = false;
        tilt_latch = false;
    }
    if (now - last_heartbeat_millis > HEARTBEAT_MILLIS) {
        if (motor_power_on && !motor_power_fault) {
            motor_power_fault = true;
            ++motor_error_count;
        }
        ROS_running = false;
    }

    const bool tiltStopped = tilt_latch && !tilt_reversing;
    const bool latched = ros_emergency || motor_power_fault || !motor_watchdog_ready ||
                         (!WHEEL_HOLD_BRAKE && tiltStopped);
    emergency_latch = stopPressed || latched || !ROS_running;
    // Mowgli wire: bit 1 STOP, bit 2 LIFT.
    status_message.emergency_bitmask = (stopPressed ? LL_EMERGENCY_BIT_STOP1 : 0) |
                                       ((latched || tiltStopped) ? LL_EMERGENCY_BIT_LATCH : 0);
    if (emergency_latch) {
        cutMotorPower();
    } else if (!motor_power_on && now - motor_power_changed >= MOTOR_POWER_REARM_MILLIS) {
        startMotorPower(now);
    }
}

void checkTiltSafety() {
    if (emergency_latch) return;
    if (tilt_latch) {
        if (tilt_reversing && millis() - tilt_reverse_start_millis >= TILT_REVERSE_MILLIS) {
            tilt_reversing = false;
#if WHEEL_HOLD_BRAKE
            drive_command_valid = false;
            left_speed_m_s = right_speed_m_s = 0;
#else
            stopDriveMotors();
#endif
        }
        return;
    }

    float pitchDeg, rollDeg;
    imu_get_tilt(&rollDeg, &pitchDeg);
    if (fabsf(rollDeg) > ROLL_STOP_DEG || pitchDeg > PITCH_STOP_DEG) {
        tilt_latch = true;
        setBladeActive(false);
        if (fabsf(rollDeg) <= ROLL_STOP_DEG && pitchDeg > PITCH_REVERSE_DEG &&
            drive_armed && drive_command_valid && millis() - last_drive_command_millis <= DRIVE_COMMAND_TIMEOUT) {
            tilt_reversing = true;
            tilt_reverse_start_millis = millis();
            left_speed_m_s = right_speed_m_s = TILT_REVERSE_MPS;
        } else {
#if WHEEL_HOLD_BRAKE
            drive_command_valid = false;
            left_speed_m_s = right_speed_m_s = 0;
#else
            stopDriveMotors();
#endif
        }
    }
}

// Po starte nechame doske jeden cely cyklus citania heartbeatov na potvrdenie RUN.
bool motorRunning(const Mm32Motor &motor, uint32_t now, uint32_t armedAt) {
    return now - armedAt < 150 || motor.axisState == MM32_RUN;
}

bool motorsConfirmed(const Mm32Motor *motors, size_t count, uint32_t now, uint32_t armedAt) {
    for (size_t i = 0; i < count; ++i) {
        if (motors[i].axisState != MM32_RUN || now - motors[i].heartbeatMillis >= now - armedAt)
            return false;
    }
    return true;
}

void checkMotorPower(uint32_t now) {
    if (!motor_power_on) return;

    bool allReady = true;
    for (const Mm32Motor &motor : wheels) {
        if (motor.errors || (drive_armed && !motorRunning(motor, now, drive_arm_millis))) {
            tripMotorPower();
            return;
        }
        allReady &= mm32Ready(motor, now) && mm32FreshOdometer(motor, now) &&
                    (motor_power_ready || motor.axisState == MM32_IDLE);
    }/*
    for (const Mm32Motor &motor : blades) {
        if (motor.errors || (blades_armed && !motorRunning(motor, now, blade_arm_millis))) {
            tripMotorPower();
            return;
        }
        allReady &= mm32Ready(motor, now) && (motor_power_ready || motor.axisState == MM32_IDLE);
    }*/
    if (!allReady) {
        if (motor_power_ready || now - motor_power_changed >= MOTOR_POWER_BOOT_TIMEOUT) tripMotorPower();
        return;
    }
    motor_power_ready = true;
}

void updateMotors(uint32_t now) {
    if (!motor_power_on) return;

    if (!tilt_reversing && drive_command_valid && now - last_drive_command_millis > DRIVE_COMMAND_TIMEOUT) {
        if (left_speed_m_s != 0 || right_speed_m_s != 0) {
            tripMotorPower();
            return;
        }
#if WHEEL_HOLD_BRAKE
        // Prikaz na rychlost 0 vyprsal: nechaj kolesa v RUN na 0 ot./s (aktivna brzda).
        drive_command_valid = false;
#else
        stopDriveMotors();
#endif
    }

    if (now - last_motor_send_millis < MOTOR_SEND_MILLIS) return;
    last_motor_send_millis = now;

    if (!motor_power_ready) {
        mm32Idle(wheelBus);
        mm32Idle(bladeBus);
        return;
    }

    const bool holdDrive =
        (WHEEL_HOLD_BRAKE && !emergency_latch) ||
        (drive_command_valid && (drive_armed || left_speed_m_s != 0 || right_speed_m_s != 0)) ||
        tilt_reversing;

    if (holdDrive) {
        if (!drive_armed) {
            for (const Mm32Motor &motor : wheels) mm32Arm(wheelBus, motor, MM32_VELOCITY);
            drive_armed = true;
            drive_arm_millis = now;
        }
        if (motorsConfirmed(wheels, 2, now, drive_arm_millis)) {
            mm32Setpoint(wheelBus, wheels[0], MM32_VELOCITY, left_speed_m_s / METERS_PER_MOTOR_TURN);
            mm32Setpoint(wheelBus, wheels[1], MM32_VELOCITY, right_speed_m_s / METERS_PER_MOTOR_TURN);
        }
    } else {
        mm32Idle(wheelBus);
    }

    if (blade_active) {
        if (!blades_armed) {
            for (const Mm32Motor &motor : blades) mm32Arm(bladeBus, motor, MM32_DUTY);
            blades_armed = true;
            blade_arm_millis = now;
        }
        if (motorsConfirmed(blades, 3, now, blade_arm_millis)) {
            float duty = blade_direction ? -BLADE_DUTY : BLADE_DUTY;
            for (const Mm32Motor &motor : blades) mm32Setpoint(bladeBus, motor, MM32_DUTY, duty);
        }
    } else {
        mm32Idle(bladeBus);
    }
}

void sendBladeStatus(uint32_t now) {
    if (!motor_power_on || now - last_blade_status_millis < BLADE_STATUS_MILLIS) return;

    float minRpm = 65535, totalAmps = 0, maxTemp = -INFINITY;
    bool allRunning = true;
    for (const Mm32Motor &motor : blades) {
        if (!mm32Ready(motor, now) || !isfinite(motor.busCurrent) || !isfinite(motor.controllerTemp) ||
            now - motor.currentMillis > MM32_FEEDBACK_TIMEOUT ||
            now - motor.tempMillis > MM32_FEEDBACK_TIMEOUT) return;
        minRpm = fminf(minRpm, 60 * fabsf(motor.turnsPerSecond));
        totalAmps += motor.busCurrent;
        maxTemp = fmaxf(maxTemp, motor.controllerTemp);
        allRunning &= motor.axisState == MM32_RUN;
    }

    ll_blade_status message = {};
    message.type = PACKET_ID_LL_BLADE_STATUS;
    message.is_active = allRunning;
    // Za chodu rozhoduje najpomalsia cepel. Pri dobehu ukaz najrychlejsiu, kym sa este toci.
    if (!allRunning) {
        minRpm = 0;
        for (const Mm32Motor &motor : blades) minRpm = fmaxf(minRpm, 60 * fabsf(motor.turnsPerSecond));
    }
    message.rpm = lroundf(fminf(minRpm, 65535));
    message.current_ma = lroundf(fminf(fmaxf(totalAmps * 1000, 0), 65535));
    message.temperature = maxTemp;
    message.error_count = motor_error_count;
    sendMessage(&message, sizeof(message));
    last_blade_status_millis = now;
}

//*************************************************************************************************************** //
///****************************************      SETUP   *********************************************************//
//****************************************************************************************************************//
void setup() {
    //  We do hardware init in this core, so that we don't get invalid states.
    //  Therefore, we pause the other core until setup() was a success
    emergency_latch = true;
    ROS_running = false;
    esp_log_level_set("*", ESP_LOG_NONE);
    pinMode(EMERGENCY_PIN, INPUT_PULLUP);
    pinMode(DOCK_PIN, INPUT);
    pinMode(MOTOR_POWER_PIN, OUTPUT);
    digitalWrite(MOTOR_POWER_PIN, LOW);
    motor_power_changed = millis();
    motor_power_fault = motorResetNeedsAck();

    lift_emergency_started = 0;
    button_emergency_started = 0;

    // Initialize messages
    imu_message = {0};
    status_message = {0};
    imu_message.type = PACKET_ID_LL_IMU;
    status_message.type = PACKET_ID_LL_STATUS;
    odometry_message.type = PACKET_ID_LL_ODOMETRY;

    // Setup pins
    init_imu();

    delay(1000);
    setRaspiPower(true);

    // init serial com to RasPi
    Serial.begin(115200, SERIAL_8N1);  // predtym SERIAL_8N1, 16, 17);  
    packetSerial.setStream(&Serial);
    packetSerial.setPacketHandler(&onPacketReceived);

    Serial1.begin(115200, SERIAL_8N1, 14, 23); // kolesa: RX, TX
    Serial2.begin(115200, SERIAL_8N1, 32, 33); // cepele: RX, TX
    gpio_pullup_en((gpio_num_t)14);
    gpio_pullup_en((gpio_num_t)32);
    mm32Idle(wheelBus);
    mm32Idle(bladeBus);

    Wire1.begin(19, 18);

    as5600.begin(); //  set direction pin.
    as5600.setDirection(AS5600_CLOCK_WISE);

    servoController.Enable();
    servoController.TurnLeft(0);
    servoController.TurnRight(0);
    motor_watchdog_ready = startMotorWatchdog();
}

void onPacketReceived(const uint8_t *buffer, size_t size) {
    if (!crcOk(buffer, size)) {
        return;
    }

    if (buffer[0] == PACKET_ID_LL_HIGH_LEVEL_CONFIG_REQ && size == sizeof(ll_config_req)) {
        ll_config_rsp response = {};
        response.type = PACKET_ID_LL_HIGH_LEVEL_CONFIG_RSP;
        response.protocol_version = MOWGLI_PROTOCOL_VERSION;
        response.active_flags = 0; // Volitelne STM32 debug funkcie tu nie su.
        response.fw_version_major = 0;
        response.fw_version_minor = 9;
        response.fw_version_patch = 3;
        sendMessage(&response, sizeof(response));
        return;
    }
    if ((buffer[0] == PACKET_ID_LL_SET_PARAM && size == 9) ||
        (buffer[0] == PACKET_ID_LL_GET_PARAM && size == 5)) {
        const uint16_t id = (uint16_t)buffer[1] | ((uint16_t)buffer[2] << 8);
        if (buffer[0] == PACKET_ID_LL_GET_PARAM && id == 0xFFFFu) {
            sendParamStoreStatus(); // GET ALL: ziadne podporovane parametre.
        } else {
            sendUnsupportedParam(id);
        }
        return;
    }
    if ((buffer[0] == PACKET_ID_LL_PARAM_COMMIT && size == 4 && buffer[1] == 0xC5) ||
        (buffer[0] == PACKET_ID_LL_PARAM_STORE_RESET && size == 8 && buffer[1] == 0xD3)) {
        param_store_last_result = 5; // PARAM_COMMIT_ERROR: flash nie je implementovana.
        sendParamStoreStatus();
        return;
    }

    if (buffer[0] == PACKET_ID_LL_HEARTBEAT && size == sizeof(struct ll_heartbeat)) {
        updateEmergency(); // skontroluj pripadny vypadok pred prepisanim casu
        // CRC and packet is OK, reset watchdog
        last_heartbeat_millis = millis();
        struct ll_heartbeat *heartbeat = (struct ll_heartbeat *) buffer;
        if (heartbeat->emergency_release_requested) ros_emergency = false;
        if (heartbeat->emergency_requested) ros_emergency = true;
        ROS_running = true;
        updateEmergency();
    } else if (buffer[0] == PACKET_ID_LL_HIGH_LEVEL_STATE && size == sizeof(struct ll_high_level_state)) {
        // copy the state
        last_high_level_state = *((struct ll_high_level_state *) buffer);
    } else if (buffer[0] == PACKET_ID_LL_CMD_VEL && size == sizeof(struct ll_cmd_vel)) {
        struct ll_cmd_vel *cmd_vel = (struct ll_cmd_vel *) buffer;

        updateEmergency();
        if (!motor_power_ready || emergency_latch || tilt_latch) return;
        if (!isfinite(cmd_vel->linear_x) || !isfinite(cmd_vel->angular_z)) {
            stopDriveMotors();
            return;
        }
        // Po napajani alebo poruche najprv cakaj na neutral, az potom povol jazdu.
        if (cmd_vel->linear_x == 0 && cmd_vel->angular_z == 0) drive_neutral_seen = true;
        if (!drive_neutral_seen) return;
        const double turn = WHEEL_DIST * double(cmd_vel->angular_z) / 2.0;
        const double left = double(cmd_vel->linear_x) - turn;
        const double right = double(cmd_vel->linear_x) + turn;
        const double limit = METERS_PER_MOTOR_TURN * WHEEL_MAX_MOTOR_RPM / 60.0;
        // Pri limite uber obom kolesam rovnakym pomerom, aby ostala rovnaka zakruta.
        const double peak = fmax(fabs(left), fabs(right));
        const double scale = peak > limit ? limit / peak : 1.0;
        left_speed_m_s = left * scale;
        right_speed_m_s = right * scale;
        last_drive_command_millis = millis();
        drive_command_valid = true;
    } else if (buffer[0] == PACKET_ID_LL_CMD_BLADE && size == sizeof(struct ll_cmd_blade)) {
        const struct ll_cmd_blade *cmd_blade = (const struct ll_cmd_blade *) buffer;
        updateEmergency();
        if (!motor_power_ready || emergency_latch || tilt_latch || cmd_blade->blade_on != 1 || cmd_blade->blade_dir > 1 ||
            (blade_active && blade_direction != cmd_blade->blade_dir)) {
            setBladeActive(false);
            return;
        }
        if (!blade_active) {
            if (millis() - blade_stop_millis < 150) return;
            for (const Mm32Motor &motor : blades) {
                // Start az ked vsetky tri dosky odpovedaju a motory stoja.
                if (!mm32Ready(motor, millis()) || motor.axisState != MM32_IDLE ||
                    fabsf(motor.turnsPerSecond) > 0.5f) return;
            }
        }
        blade_direction = cmd_blade->blade_dir;
        setBladeActive(true);
    } else if (buffer[0] == PACKET_ID_LL_REBOOT && size == 4 && buffer[1] == 0xB0) {
        // Pi si vyžiadal reset dosky.
        // Krátke zdržanie, aby sa stihla odoslať aj posledná odpoveď/ACK po seriovej linke.
        cutMotorPower();
        delay(50);
        ESP.restart();
    }
}

//*************************************************************************************************************** //
///****************************************      LOOP    *********************************************************//
//****************************************************************************************************************//
void loop() {
    updateEmergency();
    if (motor_power_on) updateMm32Feedback(millis());
    checkMotorPower(millis());
    packetSerial.update();

    uint32_t now = millis();
    updateEmergency();
    updateMotors(now);

    if (now - last_imu_millis > IMU_CYCLETIME) {
        // we have to copy to the temp data structure due to alignment issues
        imu_read(imu_temp, imu_temp + 3, imu_temp + 6);
        float pitchDeg, rollDeg;
        imu_get_tilt(&rollDeg, &pitchDeg);
        imu_message.acceleration_mss[0] = imu_temp[0];
        imu_message.acceleration_mss[1] = imu_temp[1];
        imu_message.acceleration_mss[2] = imu_temp[2];
        imu_message.gyro_rads[0] = imu_temp[3];
        imu_message.gyro_rads[1] = imu_temp[4];
        imu_message.gyro_rads[2] = imu_temp[5];
        imu_message.mag_uT[0] = imu_temp[6];
        imu_message.mag_uT[1] = imu_temp[7];
        imu_message.mag_uT[2] = imu_temp[8];

        imu_message.dt_millis = now - last_imu_millis;
        sendMessage(&imu_message, sizeof(struct ll_imu));

        last_imu_millis = now;
        checkTiltSafety();
    }

    updateEmergency();
    now = millis();
    sendBladeStatus(now);

    if (now - last_status_update_millis > STATUS_CYCLETIME) {
        if (last_high_level_state.current_mode != act_mode) {
            last_mode = act_mode;
            act_mode = last_high_level_state.current_mode;
        }

        status_message.v_battery = motor_power_on && now - mm32BatteryMillis <= MM32_FEEDBACK_TIMEOUT ? mm32BatteryVoltage : 0;
        if (!digitalRead(DOCK_PIN)) {
            status_message.v_charge = status_message.v_battery;
            status_message.charging_current = 3.0f;
            charging_allowed = true;
        } else {
            status_message.v_charge = 0.0f;
            status_message.charging_current = 0.0f;
            charging_allowed = false;
        }
        status_message.charging_current = 0.0f;

        status_message.status_bitmask = (status_message.status_bitmask & 0b11111011) | ((charging_allowed & 0b1) << 2);
        status_message.status_bitmask = (status_message.status_bitmask & 0b11011111) | ((sound_available & 0b1) << 5);
        status_message.status_bitmask |= 0b00001011;

        status_message.batt_percentage = 0; // Mowgli: bez merania SoC

        sendMessage(&status_message, sizeof(struct ll_status));

        last_status_update_millis = now;
        static uint32_t lastLeftOdometer = 0, lastRightOdometer = 0;
        if (motor_power_ready && mm32FreshEncoder(wheels[0], now) && mm32FreshEncoder(wheels[1], now) &&
            mm32FreshOdometer(wheels[0], now) && mm32FreshOdometer(wheels[1], now) &&
            wheels[0].odometerMillis != lastLeftOdometer && wheels[1].odometerMillis != lastRightOdometer) {
            lastLeftOdometer = wheels[0].odometerMillis;
            lastRightOdometer = wheels[1].odometerMillis;
            odometry_message.left_ticks = (int32_t)wheels[0].ticks;
            odometry_message.right_ticks = (int32_t)wheels[1].ticks;
            odometry_message.left_velocity_mm_s = lroundf(wheels[0].turnsPerSecond * wheels[0].direction * METERS_PER_MOTOR_TURN * 1000);
            odometry_message.right_velocity_mm_s = lroundf(wheels[1].turnsPerSecond * wheels[1].direction * METERS_PER_MOTOR_TURN * 1000);
            // Cas starsieho z dvoch merani poctu Hall krokov.
            odometry_message.acquisition_millis = now - lastLeftOdometer > now - lastRightOdometer
                                                  ? lastLeftOdometer : lastRightOdometer;
            sendMessage(&odometry_message, sizeof(struct ll_odometry));
        }
    }

    if (motor_power_ready && now - last_servo_millis > 20) {
        if (left_speed_m_s > 0.05 || left_speed_m_s < -0.05 ||
            right_speed_m_s > 0.05 || right_speed_m_s < -0.05) {
            if (left_speed_m_s - right_speed_m_s > 0.05 ||
                left_speed_m_s - right_speed_m_s < -0.05) {
                auxVar = ((left_speed_m_s + right_speed_m_s) * WHEEL_DIST) /
                         ((left_speed_m_s - right_speed_m_s) * 2 * STEER_DIST);
                if (auxVar > 0.02 || auxVar < -0.02) {
                    reqAngle = atanf(1 / auxVar) * -57.3; // nahrada arcuskotangens uhla
                } else {
                    if (left_speed_m_s < right_speed_m_s) {
                        reqAngle = 90;
                    } else {
                        reqAngle = -90;
                    }
                }
            } else {
                reqAngle = 0;
            }
        }

        actAngle = (as5600.readAngle() / 11.377);
        float dt = ((now - last_servo_millis)/1000.0f);
        float dtAngle = actAngle - actAngleOld;
        if (dtAngle>180) dtAngle = dtAngle-360;
        if (dtAngle<-180) dtAngle = dtAngle+360;
        actSpeed = dtAngle / dt;
        int actBemf = actSpeed*0.5;          // 0.60- konstanta bemf
      
        actAngleOld = actAngle;
        last_servo_millis = now;

        if (actAngle < 270 && actAngle > 90) {
            actAngle -= 180;
        } else {
            if (actAngle > 180) actAngle -= 360;
        }

        if (actAngle - reqAngle > 90 && reqAngle < 0) {
            reqAngle += 180;
        } else if (actAngle - reqAngle < -90 && reqAngle > 0) {
            reqAngle -= 180;
        }

        int act_pwm = (reqAngle - actAngle) * 3;
        // 3. Obmedzenie prúdu cez BEMF (kľúčová časť)
        const int maxCurrentPwm = 40;         // tvoja konštanta

        if (act_pwm - actBemf >  maxCurrentPwm) act_pwm = actBemf + maxCurrentPwm;
        if (act_pwm - actBemf < -maxCurrentPwm) act_pwm = actBemf - maxCurrentPwm;

        // 4. Mŕtve pásmo + riadenie
        if (act_pwm > 5) {
            act_pwm += 15;                    // kompenzácia mŕtveho pásma
            if (act_pwm > 255) act_pwm = 255;
            servoController.TurnLeft(act_pwm);
        } 
        else if (act_pwm < -5) {
            act_pwm -= 15;
            if (act_pwm < -255) act_pwm = -255;
            servoController.TurnRight(-act_pwm);
        } 
        else {
            servoController.TurnLeft(0);
            servoController.TurnRight(0);
        }

    }
    if (motor_watchdog_ready && !feedMotorWatchdog()) {
        motor_watchdog_ready = false;
        tripMotorPower();
    }
}

void sendMessage(void *message, size_t size) {
    // packages need to be at least 1 byte of type, 1 byte of data and 2 bytes of CRC
    if (size < 4) {
        return;
    }
    uint8_t *data_pointer = (uint8_t *) message;

    // calculate the CRC
    uint16_t crc = CRC16.ccitt((uint8_t *) message, size - 2);
    data_pointer[size - 1] = (crc >> 8) & 0xFF;
    data_pointer[size - 2] = crc & 0xFF;

    packetSerial.send((uint8_t *) message, size);
}

