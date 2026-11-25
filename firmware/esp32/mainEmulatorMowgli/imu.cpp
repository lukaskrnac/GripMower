#include "imu.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "BMI088.h"       // bolderflight/bmi088-arduino
#include "FastIMU.h"      // ponechané len kvôli magnetometru

// ---------------------------------------------------------------
// BMI088 - samostatné objekty pre accel a gyro (podľa oficiálneho
// príkladu knižnice), komunikácia cez I2C, polling (bez interruptov).
// ---------------------------------------------------------------
#define BMI088_ACCEL_ADDRESS 0x19   // SDO1 -> VDD (0x18 ak SDO1 -> GND)
#define BMI088_GYRO_ADDRESS  0x69   // SDO2 -> VDD (0x68 ak SDO2 -> GND)

Bmi088Accel accel(Wire, BMI088_ACCEL_ADDRESS);
Bmi088Gyro  gyro(Wire, BMI088_GYRO_ADDRESS);

// --- Magnetometer ostáva cez FastIMU ---
#define MAG_ADDRESS 0x0D
QMC5883L MAG;
calData calibMAG = { 0 };
MagData magData;

bool init_imu()
{
  Wire.begin();
  Wire.setClock(400000); //400khz clock

  int status;

  status = accel.begin();
  if (status < 0) {
    // chyba inicializácie akcelerometra - skontroluj zapojenie/adresu
    return false;
  }
  accel.setOdr(Bmi088Accel::ODR_100HZ_BW_19HZ);
  // Poznámka: pinModeInt1/mapDrdyInt1 sú potrebné len ak by si chcel
  // používať dátový interrupt pin. Pri pollingu (readSensor() v loope)
  // ich netreba volať.

  status = gyro.begin();
  if (status < 0) {
    // chyba inicializácie gyroskopu - skontroluj zapojenie/adresu
    return false;
  }
  gyro.setOdr(Bmi088Gyro::ODR_100HZ_BW_12HZ);

  MAG.init(calibMAG, MAG_ADDRESS);

  return true;
}


// ---------------------------------------------------------------
// Jednoduchá fúzia ACC + GYRO na náklon okolo X (roll) a Y (pitch)
// Komplementárny filter - ľahký na výpočet, žiadne kvaternióny.
// ---------------------------------------------------------------
static float roll_rad  = 0.0f;   // náklon okolo X
static float pitch_rad = 0.0f;   // náklon okolo Y
static unsigned long last_us = 0;

// alpha blízko 1 = viac dôveruje gyru (rýchle, ale driftuje)
// alpha blízko 0 = viac dôveruje accu (pomalé, ale bez driftu)
float ALPHA;
const float tau = 1.0f; // casova konstanta, lepsie pre nastavenie v sekundach za ktory sa ma Acc dorovnat 

bool imu_read(float *acceleration_mss, float *gyro_rads, float *mag_uT)
{
  accel.readSensor();
  gyro.readSensor();
  MAG.update();
  MAG.getMag(&magData);

  // Knižnica vracia dáta už v m/s^2 a rad/s.
  acceleration_mss[0] = accel.getAccelX_mss();
  acceleration_mss[1] = accel.getAccelY_mss();
  acceleration_mss[2] = accel.getAccelZ_mss();

  gyro_rads[0] = gyro.getGyroX_rads();
  gyro_rads[1] = gyro.getGyroY_rads();
  gyro_rads[2] = gyro.getGyroZ_rads();

  mag_uT[0] = magData.magX * 10;
  mag_uT[1] = magData.magY * 10;
  mag_uT[2] = magData.magZ * 10;

  unsigned long now_us = micros();
  float dt = (last_us == 0) ? 0.0f : (now_us - last_us) / 1000000.0f;
  last_us = now_us;
  if (dt <= 0.0f || dt > 0.5f) dt = 0.0f; // ochrana pri prvom volaní / preskoku

  // náklon vypočítaný len z accelerometra (v pokoji presný, ale citlivý na vibrácie)
  float acc_roll  = atan2f(acceleration_mss[1], acceleration_mss[2]);
  float acc_pitch = atan2f(acceleration_mss[0], acceleration_mss[2]);

  float ALPHA = tau / (tau + dt);
  // integrácia gyra + fúzia s accelerometrom
  roll_rad  = ALPHA * (roll_rad  + gyro_rads[0] * dt) + (1.0f - ALPHA) * acc_roll;
  pitch_rad = ALPHA * (pitch_rad + gyro_rads[1] * dt) + (1.0f - ALPHA) * acc_pitch;

  return true;
}


// vráti aktuálny náklon v stupnoch
void imu_get_tilt(float *roll, float *pitch)
{
  *roll  = roll_rad * 57.29578f;
  *pitch = pitch_rad * 57.29578f;
}
