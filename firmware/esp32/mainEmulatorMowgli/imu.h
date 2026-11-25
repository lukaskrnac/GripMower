#ifndef _IMU_H
#define _IMU_H

bool init_imu();

bool imu_read(float *acceleration_mss,float *gyro_rads,float *mag_uT);


void imu_get_tilt(float *roll, float *pitch);

#endif