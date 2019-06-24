# Android custom Sensor -- AdcSensor
This project will show you how to add a custom Sensor on the Android system, including how to modify the driver, hardware, framework, and how the app uses this Sensor.

## Usage
1. Copy adc to kernel/drivers/input/sensors/ directory.
2. Modify the kernel/drivers/input/sensors/Kconfig as follows:
```
source "drivers/input/sensors/adc/Kconfig"
```
3. Modify the kernel/drivers/input/sensors/Makefile as follows:
```
obj-$(CONFIG_SENSORS_ADC) += adc/
```
4. Modify the dts as follows:  
```
&adc {
	status = "okay";

	adcsensor: adcsensor {
		compatible = "adcsensor";
		io-channels = <&adc 2>;
	};
};
```
5. For how to modify hardware and framework, please refer to adcsensor.patch.

## Usage
[Download demo](https://fir.im/1a4h)  

After Android adds a custom Sensor, the APP is used in the same way as Android's other default Sensors. You only need to change the Sensor type to Sensor.TYPE_ADC.

The obtained valtage value is the original value of the ADC. It has not been calculated and converted, and the APP needs to be converted according to the correspondence between voltage and value.

The following is the implementation of monitoring the ADC Sensor voltage change, for reference only:    
```
mSensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
if (null != mSensorManager) {
    Sensor adcSensor = mSensorManager.getDefaultSensor(26); // 26: Sensor.TYPE_ADC
        if (adcSensor != null) {
            mAdcSensorEventListener = new SensorEventListener() {
                @Override
                public void onSensorChanged(SensorEvent event) {
                    float voltage = event.values[0];
                    mVoltageTv.setText(voltage + "");
                }

                @Override
                public void onAccuracyChanged(Sensor sensor, int accuracy) {

                }
            };
            mSensorManager.registerListener(mAdcSensorEventListener, adcSensor, SensorManager.SENSOR_DELAY_NORMAL);
        }
}
```
## Developed By
* ayst.shen@foxmail.com

## License
```
	Copyright  (C)  2016 - 2017 Topband. Ltd.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be a reference
    to you, when you are integrating the android gpio into your system,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.
```