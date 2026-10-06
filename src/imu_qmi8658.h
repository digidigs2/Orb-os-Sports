#pragma once
// Minimal QMI8658 (6-axis IMU) driver over I2C. Device-only. We only use the
// accelerometer's Z axis to detect "face-down" (screen toward the ground).
bool imu_begin();      // init; false if the chip isn't found
int  imu_facedown();   // 1 = face-down, 0 = not, -1 = read unavailable (don't change state)
// Was the board jolted since the last call? 1 = yes, 0 = still, -1 = read unavailable.
// A bump to the desk, a hand on the arm, picking it up: anything that moves the
// accelerometer by more than MOTION_WAKE_LSB between two reads. Call it often (every 50 ms
// or so); a knock is over in less than that and a slow poll would sample either side of it.
int  imu_motion();
