#include "ble_config.h"

#if defined(ARDUINO) 
SYSTEM_MODE(SEMI_AUTOMATIC); 
#endif

#define RECEIVE_MAX_LEN  6 // TODO: change this based on how much data you are sending from Android 
#define SEND_MAX_LEN    3

// Must be an integer between 1 and 9 and and must also be set to len(BLE_SHORT_NAME) + 1
#define BLE_SHORT_NAME_LEN 8 

// The number of chars should be BLE_SHORT_NAME_LEN - 1. So, for example, if your BLE_SHORT_NAME was 'J', 'o', 'n'
// then BLE_SHORT_NAME_LEN should be 4. If 'M','a','k','e','L','a','b' then BLE_SHORT_NAME_LEN should be 8
// TODO: you must change this name. Otherwise, you will not be able to differentiate your RedBear Duo BLE
// device from everyone else's device in class.
#define BLE_SHORT_NAME 'J','E','E','S','H','M','N'  

/* Define the pins on the Duo board
 * TODO: change and add/subtract the pins here for your applications (as necessary)
 */
#define LEFT_ANALOG_OUT_PIN D0
#define RIGHT_ANALOG_OUT_PIN D1
#define SERVO_ANALOG_OUT_PIN D2
#define TRIG_PIN D8
#define ECHO_PIN D9
#define LED_PIN D8
#define BUZZER_PIN D12

// Anything over 400 cm (23200 us pulse) is "out of range"
const unsigned int MAX_DIST = 23200;

#define MAX_SERVO_ANGLE  180
#define MIN_SERVO_ANGLE  0

#define BLE_DEVICE_CONNECTED_DIGITAL_OUT_PIN D7

// happiness meter (servo)
Servo _happinessServo;

// Device connected and disconnected callbacks
void deviceConnectedCallback(BLEStatus_t status, uint16_t handle);
void deviceDisconnectedCallback(uint16_t handle);

// UUID is used to find the device by other BLE-abled devices
static uint8_t service1_uuid[16]    = { 0x71,0x3d,0x00,0x00,0x50,0x3e,0x4c,0x75,0xba,0x94,0x31,0x48,0xf1,0x8d,0x94,0x1e };
static uint8_t service1_tx_uuid[16] = { 0x71,0x3d,0x00,0x03,0x50,0x3e,0x4c,0x75,0xba,0x94,0x31,0x48,0xf1,0x8d,0x94,0x1e };
static uint8_t service1_rx_uuid[16] = { 0x71,0x3d,0x00,0x02,0x50,0x3e,0x4c,0x75,0xba,0x94,0x31,0x48,0xf1,0x8d,0x94,0x1e };

// Define the receive and send handlers
static uint16_t receive_handle = 0x0000; // recieve
static uint16_t send_handle = 0x0000; // send

static uint8_t receive_data[RECEIVE_MAX_LEN] = { 0x01 };
int bleReceiveDataCallback(uint16_t value_handle, uint8_t *buffer, uint16_t size); // function declaration for receiving data callback
static uint8_t send_data[SEND_MAX_LEN] = { 0x00 };

// global distance variable for ultra sonic range finder.
float cm;
float inches;

// moving average parameters
const int numReadings = 10;
float readings[numReadings];      // the readings from the analog input
int readIndex = 0;                // the index of the current reading
float total = 0;                  // the running total
float average = 0;                // the average

// Define the configuration data
static uint8_t adv_data[] = {
  0x02,
  BLE_GAP_AD_TYPE_FLAGS,
  BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE, 
  
  BLE_SHORT_NAME_LEN,
  BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME,
  BLE_SHORT_NAME, 
  
  0x11,
  BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE,
  0x1e,0x94,0x8d,0xf1,0x48,0x31,0x94,0xba,0x75,0x4c,0x3e,0x50,0x00,0x00,0x3d,0x71 
};

static btstack_timer_source_t send_characteristic;
static void bleSendDataTimerCallback(btstack_timer_source_t *ts); // function declaration for sending data callback
int _sendDataFrequency = 200; // 200ms (how often to read the pins and transmit the data to Android)

// proximity and emeregency triggers.
int proximityWarning = 0;
int emergency = 0;

void setup() {
  Serial.begin(115200);
  delay(5000);
  Serial.println("Face Tracker BLE Demo.");

  // Initialize ble_stack.
  ble.init();
  
  // Register BLE callback functions
  ble.onConnectedCallback(bleConnectedCallback);
  ble.onDisconnectedCallback(bleDisconnectedCallback);

  //lots of standard initialization hidden in here - see ble_config.cpp
  configureBLE(); 
  
  // Set BLE advertising data
  ble.setAdvertisementData(sizeof(adv_data), adv_data);
  
  // Register BLE callback functions
  ble.onDataWriteCallback(bleReceiveDataCallback);

  // Add user defined service and characteristics
  ble.addService(service1_uuid);
  receive_handle = ble.addCharacteristicDynamic(service1_tx_uuid, ATT_PROPERTY_NOTIFY|ATT_PROPERTY_WRITE|ATT_PROPERTY_WRITE_WITHOUT_RESPONSE, receive_data, RECEIVE_MAX_LEN);
  send_handle = ble.addCharacteristicDynamic(service1_rx_uuid, ATT_PROPERTY_NOTIFY, send_data, SEND_MAX_LEN);

  // BLE peripheral starts advertising now.
  ble.startAdvertising();
  Serial.println("BLE start advertising.");

  // Setup pins
  pinMode(LEFT_ANALOG_OUT_PIN, OUTPUT);
  pinMode(RIGHT_ANALOG_OUT_PIN, OUTPUT);
  pinMode(BLE_DEVICE_CONNECTED_DIGITAL_OUT_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(LED_PIN,OUTPUT);
  pinMode(BUZZER_PIN,OUTPUT);

  _happinessServo.attach(SERVO_ANALOG_OUT_PIN);
  _happinessServo.write( (int)((MAX_SERVO_ANGLE - MIN_SERVO_ANGLE) / 2.0) );

  // Start a task to check status of the pins on your RedBear Duo
  // Works by polling every X milliseconds where X is _sendDataFrequency
  send_characteristic.process = &bleSendDataTimerCallback;
  ble.setTimer(&send_characteristic, _sendDataFrequency); 
  ble.addTimer(&send_characteristic);

  // initialize all the readings to 0:
  for (int thisReading = 0; thisReading < numReadings; thisReading++) {
    readings[thisReading] = 0;
}
}

/*
 * Function to calculate distance to target based on ultra sonic range finder.
*/
float CalculatedDistanceToTarget()
{
  unsigned long t1;
  unsigned long t2;
  unsigned long pulse_width;

  // Hold the trigger pin high for at least 10 us
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Wait for pulse on echo pin
  while ( digitalRead(ECHO_PIN) == 0 );

  // Measure how long the echo pin was held high (pulse width)
  // Note: the micros() counter will overflow after ~70 min

  t1 = micros();
  while ( digitalRead(ECHO_PIN) == 1);
  t2 = micros();
  pulse_width = t2 - t1;
  float cm = 0;
  float inches = 0;

  // Calculate distance in centimeters and inches. The constants
  // are found in the datasheet, and calculated from the assumed speed 
  // of sound in air at sea level (~340 m/s).
  // Datasheet: https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf
  // Print out results
  if ( pulse_width > MAX_DIST ) {
    Serial.println("Out of range");
  } else {
    cm = pulse_width / 58.0;
    inches = pulse_width / 148.0;
    Serial.print(cm);
    Serial.print(" cm \t");
    Serial.print(inches);
    Serial.println(" in");
  }

  // The HC-SR04 datasheet recommends waiting at least 60ms before next measurement
  // in order to prevent accidentally noise between trigger and echo
  // See: https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf
  delay(60);

  return cm;
}

void loop() 
{
  // If this is emergency triggered by voice or a proximity triggered by distance , turn on the signals 
  while(emergency == 1 || proximityWarning == 1)
  {
    tone(BUZZER_PIN , 1000);
    digitalWrite(LEFT_ANALOG_OUT_PIN, HIGH);
    digitalWrite(RIGHT_ANALOG_OUT_PIN, LOW);
    delay(100);
    digitalWrite(LEFT_ANALOG_OUT_PIN, LOW);
    digitalWrite(RIGHT_ANALOG_OUT_PIN, HIGH);
    delay(100);
    cm = CalculatedDistanceToTarget();
  }

  // reset if we are out of emergency mode or proximity mode.
  if(emergency == 0 || proximityWarning == 0 )
  {
    noTone(BUZZER_PIN);
    digitalWrite(LEFT_ANALOG_OUT_PIN, LOW);
    digitalWrite(RIGHT_ANALOG_OUT_PIN, LOW);
  }
  
  cm = CalculatedDistanceToTarget();
}

/*
 * Moving average smoothing function to smooth distance measurement from ultrasonic 
 * range finder.
*/
 
float smoothDistance(float inputDistance)
{
  // subtract the last reading:
  total = total - readings[readIndex];
  // read from the sensor:
  readings[readIndex] = inputDistance;
  // add the reading to the total:
  total = total + readings[readIndex];
  // advance to the next position in the array:
  readIndex = readIndex + 1;

  // if we're at the end of the array...
  if (readIndex >= numReadings) {
    // ...wrap around to the beginning:
    readIndex = 0;
  }

  // calculate the average:
  average = total / numReadings;
  Serial.print("Total ");
  Serial.println(total);
  Serial.print("Averaged ");
  Serial.println(average);

  return average;
}

/**
 * @brief Connect handle.
 *
 * @param[in]  status   BLE_STATUS_CONNECTION_ERROR or BLE_STATUS_OK.
 * @param[in]  handle   Connect handle.
 *
 * @retval None
 */
void bleConnectedCallback(BLEStatus_t status, uint16_t handle) {
  switch (status) {
    case BLE_STATUS_OK:
      Serial.println("BLE device connected!");
      digitalWrite(BLE_DEVICE_CONNECTED_DIGITAL_OUT_PIN, HIGH);
      break;
    default: break;
  }
}

/**
 * @brief Disconnect handle.
 *
 * @param[in]  handle   Connect handle.
 *
 * @retval None
 */
void bleDisconnectedCallback(uint16_t handle) {
  Serial.println("BLE device disconnected.");
  digitalWrite(BLE_DEVICE_CONNECTED_DIGITAL_OUT_PIN, LOW);
}

/**
 * @brief Callback for receiving data from Android (or whatever device you're connected to).
 *
 * @param[in]  value_handle  
 * @param[in]  *buffer       The buffer pointer of writting data.
 * @param[in]  size          The length of writting data.   
 *
 * @retval 
 */
int bleReceiveDataCallback(uint16_t value_handle, uint8_t *buffer, uint16_t size) {

  if (receive_handle == value_handle) {
    memcpy(receive_data, buffer, RECEIVE_MAX_LEN);
    Serial.print("Received data: ");
    for (uint8_t index = 0; index < RECEIVE_MAX_LEN; index++) {
      Serial.print(receive_data[index]);
      Serial.print(" ");
    }
    Serial.println(" ");

    int servoWrite;
    // process the data. 
    if (receive_data[0] == 0x01) { //receive the face data 
      // adjust location based on face width.
      int locationX = (int)receive_data[1] << 8 
                      | (int)receive_data[2];
      locationX = locationX + (receive_data[4]/2);

      // if location is less than zero then return.
      if(locationX < 0)
      {
        return 0;
      }
      
      // detect if we are in portrait or landscape and adjust resolution accordingly.
      if(receive_data[3] == 1)
      {
        if( locationX <= 480 )
        {
          servoWrite =  (locationX/480.0)*180;
        }
        else
        {
          return 0;
        }
      }
      else
      {
        if(locationX <= 620 )
        {
          servoWrite =  (locationX/620.0)*180;
        }
        else
        {
          return 0;
        }
      }

       if(receive_data[5] == 0)
       {
          servoWrite = 180 - servoWrite;
       }
    
      Serial.print("Calculated data Location and Servo Angle: ");
      Serial.print(locationX);
      Serial.print(" ");
      Serial.print(servoWrite);
      Serial.println(" ");
      _happinessServo.write(servoWrite);
    }

     // sweep the servo by the angle from voice command
      if (receive_data[0] == 0x02) { 
        int servoWrite = (int)receive_data[1];
        _happinessServo.write(servoWrite);
      }

    // set emergency mode from voice command.
     if (receive_data[0] == 0x03) { 
        emergency = (int)receive_data[1];
      }
  }
     
  return 0;
}

/**
 * @brief Timer task for sending status change to client.
 * @param[in]  *ts   
 * @retval None
 * 
 * Send the data from either analog read or digital read back to 
 * the connected BLE device (e.g., Android)
 */
static void bleSendDataTimerCallback(btstack_timer_source_t *ts) {
   // Take the distance calculated from ultra sonic range finder
   // smooth it
   float averagedDistance  = smoothDistance(cm);
   int roundedDistance  = (int)averagedDistance;

   Serial.print("Transmitted Rounded Average ");
   Serial.println(roundedDistance);

  // if greater than 50 cms( 0.5 meters) set proximity alert variable so that loop function does it's thing.
   if(roundedDistance < 50 && roundedDistance > 0)
   {
     proximityWarning = 1;
   }
   else
   {
     proximityWarning = 0;
   }

  // transmit the rounded distance to phone for display
  if(roundedDistance>0)
  {
   send_data[0] = (0x0B);
   send_data[1] = (roundedDistance >> 8);
   send_data[2] = (roundedDistance);
    if (ble.attServerCanSendPacket())
    {
        ble.sendNotify(send_handle, send_data, SEND_MAX_LEN);
        Serial.print("Sent data: ");
         for (uint8_t index = 0; index < SEND_MAX_LEN; index++) {
          Serial.print(send_data[index]);
          Serial.print(" ");
      }
      Serial.println(" ");
    }
  }
    // Restart timer.
    ble.setTimer(ts, 200);
    ble.addTimer(ts);
}
