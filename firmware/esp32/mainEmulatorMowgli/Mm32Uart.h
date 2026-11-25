#pragma once
#include <Arduino.h>
#include <string.h>

// MM32 CANSimple cez UART: A5 | ID | povel | dlzka | data | CRC XMODEM (LE).
class Mm32Uart {
public:
    struct Frame {
        uint8_t id, command, size;
        uint8_t data[8];
    };

    explicit Mm32Uart(HardwareSerial &serial) : serial(serial) {}

    void send(uint8_t id, uint8_t command, const void *data = nullptr, uint8_t size = 0) {
        uint8_t bytes[14] = {0xA5, id, command, size};
        if (size) memcpy(bytes + 4, data, size);
        uint16_t crc = crc16(bytes + 1, size + 3);
        bytes[4 + size] = uint8_t(crc);
        bytes[5 + size] = uint8_t(crc >> 8);
        serial.write(bytes, size + 6);
    }

    bool read(Frame &frame) {
        while (used || serial.available()) {
            if (!used) buffer[used++] = serial.read();
            if (buffer[0] != 0xA5) {
                discard(1);
                continue;
            }
            while (used < 4 && serial.available()) buffer[used++] = serial.read();
            if (used < 4) return false;
            uint8_t size = buffer[3];
            if (size <= 8) {
                uint8_t total = size + 6;
                while (used < total && serial.available()) buffer[used++] = serial.read();
                if (used < total) return false;
                uint16_t crc = buffer[4 + size] | (uint16_t(buffer[5 + size]) << 8);
                if (crc == crc16(buffer + 1, size + 3)) {
                    frame.id = buffer[1];
                    frame.command = buffer[2];
                    frame.size = size;
                    memcpy(frame.data, buffer + 4, size);
                    discard(total);
                    return true;
                }
            }
            discard(1);
        }
        return false;
    }

    void clearInput() {
        used = 0;
        for (int length = serial.available(); length > 0; --length) serial.read();
    }

    void request(uint8_t id, uint8_t command) {
        send(id, command | 0x80);
    }

private:
    HardwareSerial &serial;
    uint8_t buffer[14] = {};
    uint8_t used = 0;

    void discard(uint8_t count) {
        used -= count;
        memmove(buffer, buffer + count, used);
    }

    static uint16_t crc16(const uint8_t *data, uint8_t size) {
        uint16_t crc = 0;
        while (size--) {
            crc ^= uint16_t(*data++) << 8;
            for (uint8_t bit = 0; bit < 8; ++bit)
                crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
        }
        return crc;
    }
};
