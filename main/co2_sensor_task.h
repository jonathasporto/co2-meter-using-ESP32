#ifndef CO2_SENSOR_TASK_H
#define CO2_SENSOR_TASK_H

#include <stdbool.h>

void co2_sensor_power_control(bool enable);
void perform_single_measurement(void);
bool get_quick_sensor_data(int *co2, float *temp, float *hum);

#endif // CO2_SENSOR_TASK_H
