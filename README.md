# Shiny AVR Programmer

USB programmer for ATtiny25/45/85 and ATtiny24/44/84 based on STM32F103C6.

## Hardware

![render](./docs/render.png)
![schematic](./docs/schematic.png)
![signals](./docs/signals.png)

## Firmware

Firmware is based on [ArduinoISP](https://github.com/arduino/arduino-examples/blob/main/examples/11.ArduinoISP/ArduinoISP/ArduinoISP.ino) with the 
appropriate modifications for the specifics of hardware (LEDs, SPI parameters, ...) and it adds the capability to provide external clock to be able
to program ATtinys that have had their fuses set to rely on external clock or crystal.

If using `avrdude`, the programmer to use is `-c stk500v1`.