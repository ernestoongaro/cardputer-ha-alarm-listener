# Cardputer ADV smoke-alarm listener

Dedicated firmware for an M5Stack Cardputer ADV. It listens continuously for
the locally measured smoke and CO alarm signatures and exposes Home Assistant
MQTT `smoke` and `carbon_monoxide` binary sensors. It is installed as the
primary firmware: power loss, reset, or watchdog recovery boots directly back
into the listener.

Raw MQTT topics use `cardputer-alarm-<id>/#` (`<id>` is derived from the chip MAC): the retained `smoke` topic is
`OK` or `ALARM`, `carbon_monoxide` is independently `OK` or `ALARM`, `status`
is `online` or `offline`, and `telemetry` is JSON.

## Safety boundary

This is a supplementary notification device, not a certified fire detector or
life-safety system. Never remove, relocate, silence, or depend less on the real
smoke alarms. Keep their normal audible coverage and test every alarm plus this
listener after installation and at least monthly. A closed door, flat battery,
changed alarm model, microphone obstruction, or unusual acoustic environment
can prevent audio recognition.

The detector is deliberately tailored to the two installed devices:

- Smoke: a loud, harmonically rich 2.8-3.3 kHz sustained tone for three
  seconds, with only brief dropouts allowed.
- CO: three 0.28-0.85 second bursts around 2.85-3.45 kHz, with valid gaps and
  a final pause.

Those profiles come from physical TEST-button measurements. Alarm models vary,
so a real test with every smoke and CO alarm is mandatory after any change to
the listener or its location.

## Resilience behavior

- Audio detection runs locally and does not stop during Wi-Fi/MQTT outages.
- MQTT connection work is isolated on the other ESP32 core.
- The Home Assistant state and MQTT availability messages are retained.
- An alarm that could not be delivered stays latched in flash across reboots.
- It clears only after Home Assistant has had the `ON` state for at least one
  minute and no further alarm cadence has been heard for 90 seconds.
- A task watchdog automatically reboots a wedged main loop into this firmware.
- The microphone is restarted if capture stalls or returns digital silence.
- The internal battery gives short-term continuity if USB power is interrupted,
  provided the Cardputer power switch remains on and the battery is maintained.

## First boot and Home Assistant setup

1. Flash the firmware directly over USB (commands below).
2. The Cardputer creates the Wi-Fi network shown on its display. Its password is
   also shown on the display.
3. Join that network, open `http://192.168.4.1`, and enter Wi-Fi plus the IP/name,
   port, username, and password of Home Assistant's MQTT broker.
4. With MQTT discovery enabled in Home Assistant, the device
   **Cardputer Alarm Listener** and its **Smoke alarm listener** entity appear
   automatically.
5. Put the Cardputer in its final location and use the TEST button on every
   smoke and CO alarm. Confirm that Home Assistant marks the matching entity as
   `Detected`, rather than the other one.

Hold the Cardputer's top `G0` button to reopen setup later. Detection continues
while the setup access point is open.

For a fixed installation, copy `include/device_config.example.h` to
`include/device_config.h` and put the Wi-Fi/MQTT credentials there. The real
file is git-ignored and its values are authoritative at every boot. Without it
the firmware builds with the example placeholders and relies on the setup portal. Do not put
`http://` in the MQTT host. The screen sleeps after two minutes; `G0` or any
keyboard input wakes it, and an alarm wakes it at full brightness.

## Build, test, and flash

PlatformIO manages the vendor and networking libraries:

```sh
pio run -e cardputer-adv
pio run -e cardputer-adv -t upload --upload-port /dev/cu.usbmodem*
```

The signal-processing state machine is host-testable without embedded tools:

```sh
xcrun clang++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc \
  test/test_alarm_detector.cpp src/alarm_detector.cpp \
  -o /tmp/cardputer_detector_tests
/tmp/cardputer_detector_tests
```

## Installation notes

Mount it with the microphone opening unobstructed and away from speakers, TVs,
fans, extractor hoods, and direct drafts. Keep USB and its power adapter outside
places where smoke or heat would damage them early. Check that the display says
`Mic: OK`, `WiFi: OK`, and `HA/MQTT: OK`; Home Assistant also receives microphone,
signal, sound-level, and detector telemetry as attributes on the smoke entity.

## Serial console

The firmware prints `BOOT`, `STATUS` (every ~2 s), `FRAME` (while audio is above
the level gate), and `DETECT tone=… bursts=… smoke_event=… co_event=…` lines at
115200 baud. Read them with `pio device monitor`, or, if the USB port is exposed
over the network by `ser2net`, with:

```sh
python3 tools/serial_capture.py <ser2net-host> 4000 serial.log
```

The `DETECT` lines show exactly which burst, gap, or frequency check accepted or
rejected an alarm cadence, which is the fastest way to diagnose a missed test.
