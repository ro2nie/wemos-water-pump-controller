# Wemos water pump/well controller

This is for the water pump controller that switches a water pump on or off depending on wheather the water tank
intent is set to on or off, and depending on weather there is enough water in the well. The water well state has a recovery time
that can be changed from home assistant. This recovery time is to let the water well fill up with water, after it has run dry.
The water pump will immediately stop if the water well becomes empty, or the water tank stops asking for water.

In order to flash this onto a Wemos D1 mini, please rename the conectionDetails-example.h to connectionDetails.h
Then modify the credentials for the WiFi network, and the IP address of the MQTT broker.
Also modify the credentials used to authenticate with MQTT broker.