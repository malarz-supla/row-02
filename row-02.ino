/*
Copyright (C) malarz

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#define STATUS_LED_GPIO 2
#define RELAY1_GPIO 4
#define RELAY2_GPIO 5
#define SWITCH1_GPIO 12
#define SWITCH2_GPIO 13
#define BUTTON_CFG_RELAY_GPIO 0

#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/control/relay.h>
#include <supla/control/button.h>
#include <supla/control/action_trigger.h>
#include <supla/device/status_led.h>
#include <supla/storage/littlefs_config.h>
#include <supla/network/esp_web_server.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/network/html/status_led_parameters.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/network/html/select_input_parameter.h>
#include <supla/network/html/custom_text_parameter.h>
#include <supla/device/supla_ca_cert.h>
#include <supla/events.h>

Supla::ESPWifi wifi;
Supla::LittleFsConfig configSupla;

Supla::Device::StatusLed statusLed(STATUS_LED_GPIO, true); // inverted state
Supla::EspWebServer suplaServer;

#ifdef ARDUINO_ARCH_ESP32
#include <HTTPUpdateServer.h>
HTTPUpdateServer httpUpdater;
#else
#include <ESP8266HTTPUpdateServer.h>
ESP8266HTTPUpdateServer httpUpdater;
#endif

const char PARAM_IN1[] = "in1";
const char PARAM_IN2[] = "in2";
const char PARAM_TYPE[] = "inputType";
const char PARAM_NAME[] = "devName";

void setup() {
  Serial.begin(115200);

  // dodaj updater
  httpUpdater.setup(suplaServer.getServerPtr(), "/update");

  // configure defualt Supla CA certificate
  SuplaDevice.setSuplaCACert(suplaCACert);
  SuplaDevice.setSupla3rdPartyCACert(supla3rdCACert);

  // HTML www component (they appear in sections according to creation
  // sequence).
  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;
  new Supla::Html::StatusLedParameters;

  // własne konfiguracja
  Supla::Storage::Init();
  auto selectIn1 = new Supla::Html::SelectInputParameter(PARAM_IN1, "IN1");
  selectIn1->registerValue("MONOSTABLE", Supla::ON_RELEASE);
  selectIn1->registerValue("BISTABLE", Supla::ON_CHANGE);
  auto selectIn2 = new Supla::Html::SelectInputParameter(PARAM_IN2, "IN1");
  selectIn2->registerValue("MONOSTABLE", Supla::ON_RELEASE);
  selectIn2->registerValue("BISTABLE", Supla::ON_CHANGE);
  auto InputType = new Supla::Html::SelectInputParameter(PARAM_TYPE, "Monostable trigger");
  InputType->registerValue("ON RELEASE", Supla::ON_RELEASE);
  InputType->registerValue("ON PRESS", Supla::ON_PRESS);

  int32_t Button1Event = Supla::ON_CHANGE;
  int32_t Button2Event = Supla::ON_CHANGE;
  if (Supla::Storage::ConfigInstance()->getInt32(PARAM_IN1, &Button1Event)) {
    SUPLA_LOG_DEBUG(" **** Param[%s]: %d", PARAM_IN1, Button1Event);
  } else {
    SUPLA_LOG_DEBUG(" **** Param[%s] is not set", PARAM_IN1);
  }
  if (Supla::Storage::ConfigInstance()->getInt32(PARAM_IN2, &Button2Event)) {
    SUPLA_LOG_DEBUG(" **** Param[%s]: %d", PARAM_IN2, Button2Event);
  } else {
    SUPLA_LOG_DEBUG(" **** Param[%s] is not set", PARAM_IN2);
  }
  int32_t ButtonType = Supla::ON_RELEASE;
  if (Supla::Storage::ConfigInstance()->getInt32(PARAM_TYPE, &ButtonType)) {
    SUPLA_LOG_DEBUG(" **** Param[%s]: %d", PARAM_TYPE, ButtonType);
  } else {
    SUPLA_LOG_DEBUG(" **** Param[%s] is not set", PARAM_TYPE);
  }
  if (Button1Event == Supla::ON_RELEASE) {
    Button1Event = ButtonType;
    SUPLA_LOG_DEBUG(" **** Param[%s]: %d", PARAM_IN1, Button1Event);
  }
  if (Button2Event == Supla::ON_RELEASE) {
    Button2Event = ButtonType;
    SUPLA_LOG_DEBUG(" **** Param[%s]: %d", PARAM_IN2, Button2Event);
  }

  #define DEFAULT_DEV_NAME "MALARZ ROW-02"
  char devName[30] = {};
  auto DeviceName = new Supla::Html::CustomTextParameter(PARAM_NAME, "Nazwa urządzenia", 30);
  if (!DeviceName->getParameterValue(devName, 30)) {
    SUPLA_LOG_DEBUG(" **** Param[%s]: settong default %s", PARAM_NAME, DEFAULT_DEV_NAME);
    DeviceName->setParameterValue(DEFAULT_DEV_NAME);
  }
  if (Supla::Storage::ConfigInstance()->getString(PARAM_NAME, devName, 30)) {
    SUPLA_LOG_DEBUG(" **** Param[%s]: %s", PARAM_NAME, devName);
    SuplaDevice.setName(devName);
  } else {
    SUPLA_LOG_DEBUG(" **** Param[%s] is not set", PARAM_NAME);
    SuplaDevice.setName(DEFAULT_DEV_NAME);
  }

  // Channels configuration
  // CH 1 - Relay
  auto r1 = new Supla::Control::Relay(RELAY1_GPIO);
  r1->getChannel()->setDefault(SUPLA_CHANNELFNC_LIGHTSWITCH);
  auto r2 = new Supla::Control::Relay(RELAY2_GPIO);
  r2->getChannel()->setDefault(SUPLA_CHANNELFNC_LIGHTSWITCH);
  // CH 2 - Action trigger
  auto at1 = new Supla::Control::ActionTrigger();
  auto at2 = new Supla::Control::ActionTrigger();

  // Buttons configuration
  auto buttonCfgRelay = new Supla::Control::Button(BUTTON_CFG_RELAY_GPIO, true, true);
  auto Switch1Relay = new Supla::Control::Button(SWITCH1_GPIO, true, true);
  auto Switch2Relay = new Supla::Control::Button(SWITCH2_GPIO, true, true);

  buttonCfgRelay->configureAsConfigButton(&SuplaDevice);
  Switch1Relay->addAction(Supla::TOGGLE, *r1, Button1Event);
  Switch2Relay->addAction(Supla::TOGGLE, *r2, Button2Event);

  // Action trigger configuration
  at1->setRelatedChannel(r1);
  at1->attach(Switch1Relay);
  at2->setRelatedChannel(r2);
  at2->attach(Switch2Relay);

  SuplaDevice.begin();
}

void loop() {
  SuplaDevice.iterate();
}
