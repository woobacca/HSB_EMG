/*********
  Patrik Sieverding
*********/

#include <Arduino.h>
#include <WiFi.h>
#include <driver/adc.h>
#include <PubSubClient.h>

// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //
//                                                               //
//                      global definitions                       //
//                                                               //
// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //

/* Sample frequency in microseconds */
#define SAMPLE_FREQUENCY 500
/* Number of samples to send via MQTT. 
 * Tx per second = 1/(SAMPLE_FREQUENCY *10^(-6) * NUM_TX_SMAPLES)
 * sizeof(analogueSamples) * NUM_TX_SAMPLES should be smaller than 250 bytes. The used MQTT library
 * only support messages up to 256 bytes including message header which is upt to 5 bytes wide.
 */
#define NUM_TX_SAMPLES 20

/* Only a2 to a5 are usable when connectig via Wifi */
const struct ADCout {
  uint8_t a0;
  uint8_t a1;
  uint8_t a2;
  uint8_t a3;
  uint8_t a4;
  uint8_t a5;
} ADCout = {.a0 = 2, .a1 = 4, .a2 = 35, .a3 =  34, .a4 =  36, .a5 =  39};

int ledPin = 2;

/* WIFI connection info */
const char* ssid = "REPLACE WITH YOUR WLAN SSID";
const char* password = "REPLACE WITH YOUR WLAN PASSWORD";

/* IPv4 address of MQTT Broker */
const char* mqtt_server = "REPLACE WITH YOUR MQTT BROKER IPv4 ADDRESS";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

/* FIFO Queue */
QueueHandle_t sampleQueue;

/* Data container for measurements */
volatile struct analogueSamples {
  uint16_t emgValue1;
  uint16_t emgValue2;
  uint32_t time;
} analogueSamples = {.emgValue1 =0, .emgValue2=0, .time=0};

/* TIMER */
hw_timer_t* timer = NULL;
volatile SemaphoreHandle_t timerSemaphore;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED; //Mutex for timer

void ARDUINO_ISR_ATTR onTimer() {
  // Atomic read of ADC 
  portENTER_CRITICAL_ISR(&timerMux);
  analogueSamples.emgValue1 = analogRead(ADCout.a2);
  analogueSamples.emgValue2 = analogRead(ADCout.a3);
  analogueSamples.time = micros();
  portEXIT_CRITICAL_ISR(&timerMux);
  // Give a semaphore that we can check in the loop
  xSemaphoreGiveFromISR(timerSemaphore, NULL);
  // It is safe to use digitalRead/Write here if you want to toggle an output
}

// Taskhandle for task running on Core 0
TaskHandle_t Task1;


// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //
//                                                               //
//                     function definitions                      //
//                                                               //
// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
 
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

/* Function for reconnecting to the MQTT broker. Is run in setup() to connect before sampling loop
 * starts.
 */
void mqtt_reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("esp32_e8:6b:ea:d8:fc:d4")) {
      digitalWrite(ledPin, HIGH);
      Serial.println("connected");
      // Subscribe
      client.subscribe("esp32/output");
    } else {
      digitalWrite(ledPin,LOW);
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void mqtt_callback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  /* If a message is received on the topic esp32/output, you check if the message is either "on" or "off". 
   * Changes the output state according to the message
   */
  if (String(topic) == "esp32/output") {
    Serial.print("Changing output to ");
    if(messageTemp == "on"){
        Serial.println("on");
        /* set to 1MHz resolution */
        timer = timerBegin(1000000);
        /* Attach onTimer function to our timer. */
        timerAttachInterrupt(timer, &onTimer);
       /* Set alarm to call onTimer function every second (value in microseconds).
        * Repeat the alarm (third parameter) with unlimited count = 0 (fourth parameter).
        */
       timerAlarm(timer, SAMPLE_FREQUENCY, true, 0);
    }
    else if(messageTemp == "off"){
      Serial.println("off");
      timerEnd(timer);
    }
  }
}
// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //
//                                                               //
//                   setup and initialization                    //
//                                                               //
// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //


void setup() {
  Serial.begin(115200); 
  pinMode(ledPin, OUTPUT);
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqtt_callback);
  void mqtt_reconnect();

  sampleQueue = xQueueCreate(20, sizeof(struct analogueSamples));
  if(sampleQueue == NULL) {
  }

  //create a task that will be executed in the Task1code() function, with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
                    dataProcessing,   /* Task function. */
                    "dataProcessing",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &Task1,      /* Task handle to keep track of created task */
                    0);          /* pin task to core 0 */                  
  delay(500);  // needed to start-up task1

  // Create semaphore to inform us when the timer has fired
  timerSemaphore = xSemaphoreCreateBinary();
}


// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //
//                                                               //
//                       Core 1: Sample get                      //
//                                                               //
// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //


void loop() {
  if (xSemaphoreTake(timerSemaphore, 0) == pdTRUE) {
    /* Read the interrupt and send sample data to FIFO */
    xQueueSend(sampleQueue, (void*) &analogueSamples, (TickType_t)10);
  }
}


// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //
//                                                               //
//                   Core 0:  Sample Processing                  //
//                                                               //
// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||| //


void dataProcessing( void * pvParameters ){

  uint8_t i = 0;
  struct analogueSamples txSamples[NUM_TX_SAMPLES];
  uint8_t serializedData[sizeof(txSamples)];

  uint8_t txLength = sizeof(txSamples)/sizeof(txSamples[0]);

  for(;;){

    if (!client.connected()) {
      mqtt_reconnect();
    }
    client.loop();

    if( xQueueReceive(sampleQueue, &txSamples[i], (TickType_t) 10)  == pdPASS) { 

      i++;
      if (i == txLength) {
        /* The MQTT library can only send char or byte arrays. Because of this we need to copy the
         * txSamples array into a buffer of type uint8_t */
        memcpy(serializedData, txSamples, sizeof(serializedData));
        client.publish("esp32/emg1", serializedData, sizeof(serializedData));
        i = 0;
      }
    }
  } 
}