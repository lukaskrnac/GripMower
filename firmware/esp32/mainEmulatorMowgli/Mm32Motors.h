#pragma once
#include "Mm32Uart.h"
#include <math.h>

// Nastavenia pre tvoju kosacku.
constexpr float WHEEL_DIAMETER_M = 0.29f;
constexpr float WHEEL_GEAR_RATIO = 9.7f;            // otacky motora / otacky kolesa
constexpr float WHEEL_MAX_MOTOR_RPM = 1000.0f;      // limit Set_Input_Vel vo firmware MM32
constexpr float BLADE_DUTY = 0.20f;                // 0..1, vyskusal by som najprv bez nozov
constexpr float METERS_PER_MOTOR_TURN = 3.14159265f * WHEEL_DIAMETER_M / WHEEL_GEAR_RATIO;

enum Mm32Command : uint8_t {
    MM32_HEARTBEAT = 0x01, MM32_RX_SDO = 0x04, MM32_TX_SDO = 0x05,
    MM32_AXIS_STATE = 0x07, MM32_ENCODER = 0x09,
    MM32_CONTROL_MODE = 0x0B, MM32_VELOCITY = 0x0D, MM32_TEMPERATURE = 0x15,
    MM32_BUS_VOLTAGE = 0x17,
    MM32_DUTY = 0x1E
};

// SDO citanie hoverboard.odometer (0x0109): operacia READ, ID little endian, rezerva.
constexpr uint8_t MM32_ODOMETER_READ[] = {0, 0x09, 0x01, 0};
constexpr uint32_t MM32_FEEDBACK_TIMEOUT = 250;
constexpr uint32_t MM32_IDLE = 1;
constexpr uint32_t MM32_RUN = 8;

struct Mm32Motor {
    Mm32Motor(uint8_t id, int8_t direction) : id(id), direction(direction) {}

    uint8_t id;
    int8_t direction;
    bool hasHeartbeat = false;
    bool hasEncoder = false;
    uint32_t heartbeatMillis = 0;
    uint32_t encoderMillis = 0;
    uint32_t errors = 0;
    uint8_t axisState = 0;
    float turnsPerSecond = 0;
    float busCurrent = NAN, controllerTemp = NAN;
    uint32_t currentMillis = 0, tempMillis = 0;
    bool hasOdometer = false;
    uint32_t odometerMillis = 0;
    uint32_t lastRawTicks = 0;
    uint32_t ticks = 0; // suctom celych Hall krokov od startu ESP32, so znamienkom podla smeru
};

Mm32Uart wheelBus(Serial1);
Mm32Uart bladeBus(Serial2);
Mm32Motor wheels[] = {{1, 1}, {2, -1}};            // lave, prave
Mm32Motor blades[] = {{3, 1}, {4, 1}, {5, 1}};     // zmen smer konkretneho motora na -1 podla montaze
constexpr uint8_t BATTERY_NODE = 1;                 // doska kolies s funkcnym meranim VBAT
float mm32BatteryVoltage = 0;
uint32_t mm32BatteryMillis = 0;
uint32_t mm32PollMillis = 0;

inline bool mm32FreshEncoder(const Mm32Motor &motor, uint32_t now) {
    return motor.hasEncoder && now - motor.encoderMillis <= MM32_FEEDBACK_TIMEOUT;
}

inline bool mm32FreshOdometer(const Mm32Motor &motor, uint32_t now) {
    return motor.hasOdometer && now - motor.odometerMillis <= MM32_FEEDBACK_TIMEOUT;
}

inline bool mm32Ready(const Mm32Motor &motor, uint32_t now) {
    return mm32FreshEncoder(motor, now) && motor.hasHeartbeat &&
           now - motor.heartbeatMillis <= MM32_FEEDBACK_TIMEOUT && motor.errors == 0;
}

inline void readMm32(Mm32Uart &bus, Mm32Motor *motors, uint8_t count, uint32_t now, bool driveBus) {
    Mm32Uart::Frame packet;
    while (bus.read(packet)) {
        if (packet.size != 8) continue;
        for (uint8_t i = 0; i < count; ++i) {
            Mm32Motor &motor = motors[i];
            if (motor.id != packet.id) continue;
            if (packet.command == MM32_HEARTBEAT) {
                uint32_t errors;
                memcpy(&errors, packet.data, 4);
                motor.errors |= errors; // zachyt aj kratku chybu pred dalsim heartbeat
                motor.axisState = packet.data[4];
                motor.heartbeatMillis = now;
                motor.hasHeartbeat = true;
            } else if (packet.command == MM32_ENCODER) {
                float speed;
                memcpy(&speed, packet.data + 4, 4);
                if (!isfinite(speed) || fabsf(speed) > (driveBus ? 100.0f : 10000.0f)) continue;
                motor.turnsPerSecond = speed;
                motor.encoderMillis = now;
                motor.hasEncoder = true;
            } else if (driveBus && packet.command == MM32_TX_SDO &&
                       memcmp(packet.data, MM32_ODOMETER_READ, sizeof(MM32_ODOMETER_READ)) == 0) {
                uint32_t rawTicks;
                memcpy(&rawTicks, packet.data + 4, 4);
                if (mm32FreshOdometer(motor, now)) {
                    // Rozdiel funguje aj pri preteceni 32-bitoveho pocitadla MM32.
                    int32_t delta = (int32_t)(rawTicks - motor.lastRawTicks);
                    // Po resete neber velky skok ako pohyb. MM32 podporuje najviac 200 polov (600
                    // krokov/ot.) a povel do 1000 RPM (10 krokov/ms). Pridaj rezervu 1/4 otacky.
                    int32_t maxDelta = 150 + 10 * (now - motor.odometerMillis);
                    if (delta >= -maxDelta && delta <= maxDelta) {
                        if (motor.direction > 0) motor.ticks += (uint32_t)delta;
                        else motor.ticks -= (uint32_t)delta;
                    }
                }
                motor.lastRawTicks = rawTicks;
                motor.odometerMillis = now;
                motor.hasOdometer = true;
            } else if (packet.command == MM32_BUS_VOLTAGE) {
                float volts, amps;
                memcpy(&volts, packet.data, 4);
                memcpy(&amps, packet.data + 4, 4);
                if (driveBus && packet.id == BATTERY_NODE && isfinite(volts) && volts > 0) {
                    mm32BatteryVoltage = volts;
                    mm32BatteryMillis = now;
                } else if (!driveBus && isfinite(amps)) {
                    motor.busCurrent = amps;
                    motor.currentMillis = now;
                }
            } else if (!driveBus && packet.command == MM32_TEMPERATURE) {
                float temperature;
                memcpy(&temperature, packet.data, 4);
                if (isfinite(temperature)) {
                    motor.controllerTemp = temperature;
                    motor.tempMillis = now;
                }
            }
        }
    }
}

inline void updateMm32Feedback(uint32_t now) {
    readMm32(wheelBus, wheels, 2, now, true);
    readMm32(bladeBus, blades, 3, now, false);

    // Na kazdom UARTe len jedna otazka naraz, aby dve dosky neodpovedali sucasne.
    if (now - mm32PollMillis < 10) return;
    mm32PollMillis = now;
    static uint8_t wheelPoll = 0, bladePoll = 0;
    if (wheelPoll < 2)
        wheelBus.send(wheels[wheelPoll].id, MM32_RX_SDO, MM32_ODOMETER_READ, sizeof(MM32_ODOMETER_READ));
    else if (wheelPoll < 6)
        wheelBus.request(wheels[wheelPoll % 2].id, wheelPoll < 4 ? MM32_ENCODER : MM32_HEARTBEAT);
    else
        wheelBus.request(BATTERY_NODE, MM32_BUS_VOLTAGE);
    constexpr uint8_t bladeRequests[] = {MM32_ENCODER, MM32_HEARTBEAT, MM32_BUS_VOLTAGE, MM32_TEMPERATURE};
    bladeBus.request(blades[bladePoll % 3].id, bladeRequests[bladePoll / 3]);
    wheelPoll = (wheelPoll + 1) % 7;
    bladePoll = (bladePoll + 1) % 12;
}

inline void mm32Setpoint(Mm32Uart &bus, const Mm32Motor &motor, uint8_t command, float value) {
    const float data[] = {value * motor.direction, 0.0f}; // pri rychlosti je druhy float torque feedforward
    bus.send(motor.id, command, data, command == MM32_VELOCITY ? 8 : 4);
}

inline void mm32Arm(Mm32Uart &bus, const Mm32Motor &motor, uint8_t command) {
    const uint32_t mode[] = {command == MM32_VELOCITY ? 2u : 0u, 1u}; // velocity/PWM, passthrough
    bus.send(motor.id, MM32_CONTROL_MODE, mode, sizeof(mode));
    mm32Setpoint(bus, motor, command, 0);
    bus.send(motor.id, MM32_AXIS_STATE, &MM32_RUN, sizeof(MM32_RUN));
}

inline void mm32Idle(Mm32Uart &bus) {
    bus.send(0x3F, MM32_AXIS_STATE, &MM32_IDLE, sizeof(MM32_IDLE));
}
