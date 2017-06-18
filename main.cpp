/* Cut a CAN bus in 2 and insert a device to filter the messages.
 *
 * This app uses an Electron with 2 CAN controllers to reads CAN
 * messages from one side of the bus to forward them to the other side
 * of the bus. When certain CAN message IDs match, it modifies or drops
 * the message
 */
//#include <Adafruit_SSD1306.h>
/* Transmit the value of some knobs connected to the Carloop as CAN messages
 */

#include "application.h"
#include "clickButton.h"
//#include "carloop.h"
//#include "socketcan_serial.h"

SYSTEM_THREAD(ENABLED);

bool filterCAN(CANMessage &m);
CANMessage modifyData(int i);
void transmitCAN();
bool appendToList(CANMessage &m);
void sortIDs();
void socketcanReceiveMessages();
void printReceivedMessage(const CANMessage &message);
void doEncoderA();
void doEncoderB();
void enableAll();
void listIDs(int i);
void refresh();
void displayMenu();


int encoderA = D4;
int encoderB = D5;

volatile bool A_set = false;
volatile bool B_set = false;
volatile int encoderPos = 0;
int prevPos = 0;
int updown = 0;

int selectButtonPin = 3;
ClickButton selectButton(selectButtonPin, LOW, CLICKBTN_PULLUP);
int selection = 0;
int led = D7;

#define RST "\033[0m"
#define SEL "\033[1;93;41m"
#define CHANGING "\033[1;93;31m"
#define INVERT "\033[7m"
#define BLINK "\033[5m"

#define REFESH_RATE 500
#define SCREEN_BUFFER 26

static int sel = 0;
static int data_select = 0;

long timeOfLastRefresh = 0;

// The 2 CAN controllers on the Electron
CANChannel can1(CAN_D1_D2);
CANChannel can2(CAN_C4_C5);

const char NEW_LINE = '\r';
char inputBuffer[40];
unsigned inputPos = 0;

CANMessage spoofM;
//CANMessage CANrecord[1];
//int recording = 0;
//int recordCount = 0;

// Modify if your CAN bus has a different speed / baud rate
const auto speed = 500000; // bits/s
PMIC _pmic;

bool enableAllState = true;

struct dataInfo {
  bool enable = false;
  bool modify = false;
  uint8_t current = 0x00;
  uint8_t previous = 0x00;
  uint8_t modified = 0x00;
};

struct IDinfo {
  bool enable = false;
  bool modify = false;
  int id;
  int frequency; 
  dataInfo data[8];
};

IDinfo IDlist[1700];

int IDcount = 0;

/* every
 * Helper than runs a function at a regular millisecond interval
 */
template <typename Fn>
void every(unsigned long intervalMillis, Fn fn) {
  static unsigned long last = 0;
  if (millis() - last > intervalMillis) {
    last = millis();
    fn();
  }
}

void setup() {

  // Connect to CAN
  can1.begin(speed);
  //carloop.begin();    // for recieving in socketcan 
  can2.begin(speed);  // for transmitting from deciever

  // Setup serial connections 
  Serial.begin(9600);
  USBSerial1.begin(9600);

  while (!USBSerial1) delay(100);

  _pmic.disableCharging();

  // Setup encoder knobs
  pinMode(encoderA, INPUT_PULLUP);
  pinMode(encoderB, INPUT_PULLUP);
  attachInterrupt(encoderA, doEncoderA, CHANGE);
  attachInterrupt(encoderB, doEncoderB, CHANGE);

  // Setup buttons
  pinMode(D3, INPUT_PULLUP);
  pinMode(led, OUTPUT);
  selectButton.debounceTime   = 30;   // Debounce timer in ms
  selectButton.multiclickTime = 250;  // Time limit for multi clicks
  selectButton.longClickTime  = 1000; // time until "held-down clicks" register

  // Refresh menu interface
  refresh();

}

void loop() {
  
  CANMessage m;
  CANMessage m2;

  socketcanReceiveMessages();

  

  if (can2.receive(m)) {
      appendToList(m);
      sortIDs();
  }

  transmitCAN();


  // Check button and encoder to update menu

  selectButton.Update();

  if(selectButton.clicks == 1){
      selection = 1;
      displayMenu();
  }

  if( prevPos != encoderPos ) {
      if( prevPos > encoderPos ){
          updown = 1;
      }else{
          updown = 0;
      }
      prevPos = encoderPos;
      displayMenu();
  }

  // Automatically refresh menu every so often (could use 'every' template)

  if( (millis() - timeOfLastRefresh) > REFESH_RATE){
      refresh();
  }
}

// Modify or drop the messages intercepted by the man-in-the-middle device
bool filterCAN(CANMessage &m) {
  // return false to drop the message
  for( int i = 0 ; i < IDcount ; i++){
    if( m.id == IDlist[i].id ){
      return IDlist[i].enable;
    }
  }
  return false;
}

CANMessage modifyData(int i){
  CANMessage m;
  m.id = IDlist[i].id;
  m.len = 8;
    for( int j = 0 ; j < 8 ; j++){
      if( IDlist[i].data[j].enable ){
        m.data[j] = IDlist[i].data[j].modified;
      }else{
        m.data[j] = IDlist[i].data[j].current;
      }
    }
    printReceivedMessage(m);
    return m;
}


/*CANMessage blockData(int i){
  CANMessage m;
  m.id = IDlist[i].id;
  m.len = 8;
    for( int j = 0 ; j < 8 ; j++){
        m.data[j] = IDlist[j].data[j].previous;
    }
  return m;
}

void spoofData(){
  // write to the data array to modify the message
    spoofM.id =  0x456;
    spoofM.len = 8;
    spoofM.data[0] = 0x80;
    spoofM.data[1] = 0x80;
    spoofM.data[2] = 0x80;
    spoofM.data[3] = 0x80;
    spoofM.data[4] = 0xFF;
    spoofM.data[5] = 0x80;
    spoofM.data[6] = 0x80;
    spoofM.data[7] = 0x80;
    //if(spoofM.id >= 0x7FF){
      //spoofM.id = 0x000;
    //}
}
*/

void transmitCAN() {
  every(10, [] {

    for( int i = 0 ; i < IDcount ; i++){
      if( IDlist[i].enable ){
          can1.transmit(modifyData(i));
      }else{
        //can1.transmit(blockData(i));
      }
    }

  });
}


bool appendToList(CANMessage &m){
    
    int match = 0;
    int trip = 1;

    if( IDcount > 1699){
      return false;
    }

    for ( int i = 0 ; i < IDcount ; i++){
      if(m.id == IDlist[i].id){
          trip--;
          match = i;
      }
    }
    if (trip == 1){
          IDcount++;
          match = IDcount-1;
             IDlist[match].id = m.id;
    }
    IDlist[match].frequency++;
    for( int j = 0 ; j < 8 ; j++){
        IDlist[match].data[j].current = m.data[j];
    }
    return trip;
}

void sortIDs(){
  //Sort by priority
  struct IDinfo IDswap;
  int i, j;
  for ( i = 0 ; i < IDcount - 1 ; i++){
      for ( j = 0 ; j < IDcount - i - 1 ; j++){
        if(IDlist[j].id > IDlist[j+1].id){
            IDswap = IDlist[j];
            IDlist[j] = IDlist[j+1];
            IDlist[j+1] = IDswap;
        }
      }
  }
  //Sort by frequency
  /*for ( i = 0 ; i < IDcount - 1 ; i++){
      for ( j = 0 ; j < IDcount - i - 1 ; j++){
        if(IDlist[j].frequency < IDlist[j+1].frequency){
            IDswap = IDlist[j];
            IDlist[j] = IDlist[j+1];
            IDlist[j+1] = IDswap;
        }
      }
  }*/
}


// from socketcan_serial.cpp

void socketcanReceiveMessages(){
  CANMessage message;
  while(can1.receive(message))
  {
    printReceivedMessage(message);
    //applicationCanReceiver(message);
  }
}

void printReceivedMessage(const CANMessage &message){
  Serial.printf("t%03x%d", message.id, message.len);
  for(auto i = 0; i < message.len; i++) {
    Serial.printf("%02x", message.data[i]);
  }
  Serial.write(NEW_LINE);
}


// Encoder logic (should probably move a header, especially if more than one)
void doEncoderA(){
    if( digitalRead(encoderA) != A_set ){
        A_set = !A_set;
        // adjust counter + if A leads B
        if ( A_set && !B_set ) 
          encoderPos += 1;
    }
}

// Interrupt on B changing state, same as A above
void doEncoderB(){
    if( digitalRead(encoderB) != B_set ) {
        B_set = !B_set;
        //  adjust counter - 1 if B leads A
        if( B_set && !A_set ) 
          encoderPos -= 1;
    }
}


void enableAll(){

  for( int i = 0 ; i < IDcount ; i++){
      if( enableAllState ){
        IDlist[i].enable = IDlist[i].enable ? IDlist[i].enable : !IDlist[i].enable;
      }else{
        IDlist[i].enable = IDlist[i].enable ? !IDlist[i].enable : IDlist[i].enable;
      }
  }
  enableAllState = !enableAllState;

}

void listIDs(int i){

    if( sel == i+1 ){
        USBSerial1.print(SEL);    //highlight selected line
        if( data_select == 0 ){
            USBSerial1.print(INVERT); //highlight selected ID
        }
    }
    USBSerial1.print(IDlist[i].enable ? "   [X]   " : "   [ ]   ");
    USBSerial1.printf("      %03x    ", IDlist[i].id);
    USBSerial1.print(RST);

    if(!IDlist[i].enable){ 
        for( int j = 0 ; j < 8 ; j++){
            USBSerial1.print(RST);
            USBSerial1.printf("%02x ", IDlist[i].data[j].previous);
        }
    }else{
        for( int j = 0 ; j < 8 ; j++){
            if(!IDlist[i].data[j].enable){
              if( sel == i+1 && data_select == j+1 ){
                  USBSerial1.print(INVERT); //highlight selected byte
              }
              if(IDlist[i].data[j].current != IDlist[i].data[j].previous){
                  USBSerial1.print(CHANGING);
                  USBSerial1.printf("%02x ", IDlist[i].data[j].current);
              }else{
                 USBSerial1.print(sel == i+1 ? SEL : RST);
                 USBSerial1.printf("%02x ", IDlist[i].data[j].current);
              }
              IDlist[i].data[j].previous = IDlist[i].data[j].current;
            }else{
              USBSerial1.print(INVERT);  //highlight modified data
              USBSerial1.printf("%02x ", IDlist[i].data[j].modified);
              //USBSerial1.print(sel == i+1 ? SEL : RST);
            }
        USBSerial1.print(RST);
        }
    }
  USBSerial1.print(RST);
  USBSerial1.println();
      
}

void refresh() {

    USBSerial1.print("\033[2J"); // clear screen
    USBSerial1.print("\033[H"); // cursor to home
    
    if( sel == 0 ){
        USBSerial1.print(SEL);
        USBSerial1.print(INVERT);
    }

    USBSerial1.print(enableAllState ? " [ ] " : " [X] ");
    USBSerial1.print("Enabled ");
    USBSerial1.print(RST);
    USBSerial1.println("|  ID  |          Data           ");

    if( sel < SCREEN_BUFFER ){
        for( int i = 0 ; i < SCREEN_BUFFER && i < IDcount ; i++ ){
            listIDs(i);
        }
    }else{
        for( int i = 0 ; i < SCREEN_BUFFER && i < IDcount + i - SCREEN_BUFFER ; i++ ){
            listIDs(sel + i - SCREEN_BUFFER);
        }
    }
    USBSerial1.print(RST);
    timeOfLastRefresh = millis();
}

bool scroll_data = false;
bool scroll_list = true;

void displayMenu() {

    if(selection == 1){  // push button pushed

        if( sel == 0 ){  
            enableAll(); 
        }else{
            if( scroll_list ){ 

                if( !IDlist[sel-1].enable ){

                    scroll_list = 0;
                    scroll_data = 1;
                    IDlist[sel-1].enable = 1;

                }else 
                if( IDlist[sel-1].enable ){

                    IDlist[sel-1].enable = 0;
                }

            }else 
            if ( scroll_data ){

                if( data_select == 0){
                    scroll_list = 1;
                    scroll_data = 0;
                }else
                if( !IDlist[sel-1].data[data_select-1].modify && !IDlist[sel-1].data[data_select-1].enable ){
                    IDlist[sel-1].data[data_select-1].modify = 1;
                    IDlist[sel-1].data[data_select-1].enable = 1;
                    IDlist[sel-1].data[data_select-1].modified = IDlist[sel-1].data[data_select-1].current;
                }else 
                if( !IDlist[sel-1].data[data_select-1].modify && IDlist[sel-1].data[data_select-1].enable ){
                    IDlist[sel-1].data[data_select-1].enable = 0;
                }
                else 
                if( IDlist[sel-1].data[data_select-1].modify && IDlist[sel-1].data[data_select-1].enable){
                    IDlist[sel-1].data[data_select-1].modify = 0;
                }
            }
        }
        selection = 0;

    }else if( updown == 1 ){ // going up list
        
        if( scroll_list ){

            sel = sel <= 0 ? 0 : (sel - 1);

        }else 
        if ( scroll_data ){

            if(IDlist[sel-1].data[data_select-1].modify){
                IDlist[sel-1].data[data_select-1].modified -= 0x01;
            }else{
                data_select = data_select <= 0 ? 0 : (data_select - 1);
            }
        }

    }else if( updown == 0 ){ // going down list

        if( scroll_list ){

            sel = sel >= IDcount ? IDcount : (sel + 1);

        }else 
        if( scroll_data ){

            if( data_select == 0){
                data_select = 1;
            }else
            if(IDlist[sel-1].data[data_select-1].modify){
                IDlist[sel-1].data[data_select-1].modified += 0x01;
            }
            else{
                data_select = data_select >= 8 ? 8 : (data_select + 1);
            }
        }
        
    }

    refresh();
    
}