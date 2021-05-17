#include <Arduino.h>
#include <WiFi.h>
#include "ESP32_MailClient.h"
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <esp32-hal-timer.h>

/****************************************************************************************/
/************************DEFINICIONES DE CONFIGURACION***********************************/
/****************************************************************************************/

#define WIFI_SSID "valmei"
#define WIFI_PASSWORD "valmeilab"

#define GMAIL_SMTP_SERVER "smtp.gmail.com"
#define GMAIL_SMTP_PORT 465
#define OUTLOOK_SMTP_SERVER "smtp.office365.com"
#define OUTLOOK_SMTP_PORT 587

/* Credenciales e-mail */
#define GMAIL_SMTP_USERNAME "marcosrc1992.inycom@gmail.com"
#define GMAIL_SMTP_PASSWORD "Cc985090785"

/* ASIGNACIONES DE PINES DEL ESP32*/
#define P_LEDWIFI 18  //modo OUTPUT (Digital)
#define P_ACK 19      //modo INPUT_PULLUP con ISR a flanco descencente (Digital)
#define P_LEDOK 17    //modo OUTPUT (Digital)
#define P_LEDALM 16   //modo OUTPUT (Digital)
#define P_BUZZALM 12  //modo OUTPUT (Digital)
//La Pt100 se encuentra en el 34

#define N_TIMERMAX 480  //tiempo en medias horas para que salte la alarma
#define N_INTENTOS_WIFI_MAX 30

#define BOT_TOKEN "1769477105:AAEoHzDoJ5YO7-LEhH_NC4aoRSqDFrxcKGw"
#define NUM_USERS 2

/****************************************************************************************/
/************************VARIABLES DE CONFIGURACION**************************************/
/****************************************************************************************/

bool en_mails = 1; //permiso de mandar emails, se modifica solo aqui. 0 = deshabilita; 1 = habilita

//un array para comprobar si es usario autorizado se debe modificar NUM_USERS en los #define dependiendo del numero de usuarios autorizados
String CHAT_ID[NUM_USERS] = {"1769646176", "1395683047"};

/****************************************************************************************/
/****************************************************************************************/
/****************************************************************************************/

/* Declaracion del objeto de sesion SMTP */
SMTPData data;

//Declaracion de funcion de interrupción de pulsacion
void IRAM_ATTR ISR_ACK();
volatile bool flag_ACK = 0;

//Declaración la de funcion de estados
void maquina_estados(int);
void estados_automaticos();
int estado = 0;

// Analog ADC1_CH6 incializacion
const int P_pt100 = 34;
float pt100Value = 0;
float temp_actual = 0;
float temp_preop = 0;

//variables de estado
bool initW = 0;
bool estado_arranque = 0;
bool estado_OK = 0;
bool estado_ACK = 0;
bool estadoLEDWIFI = LOW; //LOW = 0x0
bool estado_revision = 0;
volatile bool parp1Hz = LOW; //LOW = 0x0

//nombre del remitente
char* remitente = "ESP32";
char* asunto = "Actualizacion estado frigorifico";

//configuracion del timer
hw_timer_t * timer = NULL;
void IRAM_ATTR onTimer(); //Declaracion de funcion de interrupción de timer
volatile long interruptCounter = 0;

hw_timer_t * timer_parp = NULL;
void IRAM_ATTR parp_mantenimiento(); //Declaracion de funcion de interrupción de timer

hw_timer_t * timer_wifi = NULL;
void IRAM_ATTR check_wifi(); //Declaracion de funcion de interrupción de timer

volatile bool flag_alarma = 0;
volatile bool flag_timer = 0;

//Funcion de manejo de mensajes de telegram
void handleNewMessages(int);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
// Checks for new messages every 1 second.
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;


/****************************************************************************************/
/******************************FUNCION ENVIO E-MAILS*************************************/
/****************************************************************************************/

String sendEmail(char *subject, char *sender, char *body, char *recipient, boolean htmlFormat) {
  data.setLogin(GMAIL_SMTP_SERVER, GMAIL_SMTP_PORT, GMAIL_SMTP_USERNAME, GMAIL_SMTP_PASSWORD);
  data.setSender(sender, GMAIL_SMTP_USERNAME);
  data.setSubject(subject);
  data.setMessage(body, htmlFormat);
  data.addRecipient(recipient);
  if (!MailClient.sendMail(data))
    return MailClient.smtpErrorReason();

  return "";
}

/****************************************************************************************/
/****************************************************************************************/
/****************************************************************************************/

void setup() {

  pinMode(P_ACK, INPUT_PULLUP);
  pinMode(P_LEDWIFI, OUTPUT);
  pinMode(P_LEDOK, OUTPUT);
  pinMode(P_LEDALM, OUTPUT);
  pinMode(P_BUZZALM, OUTPUT);
   
  //config del pin de interrupcion de ACK
  attachInterrupt(P_ACK, ISR_ACK, FALLING);

  Serial.begin(115200);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    //parpadeo en LED AZUL
    digitalWrite(P_LEDWIFI, estadoLEDWIFI); 
    if (estadoLEDWIFI == LOW) 
      estadoLEDWIFI = HIGH; 
    else
      estadoLEDWIFI = LOW;
    Serial.print(".");
    delay(140);
  }

  //fijar parpadeo del LED
  digitalWrite(P_LEDWIFI, HIGH);
  Serial.println("WiFi connected.");

  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  //setup del timer 0 interrupcion lectura ADC
  timer = timerBegin(0, 65536, true);       //timer 0 PS = 65536 f=1220Hz countUp=true
  timerAttachInterrupt(timer, &onTimer, true);
  //timerAlarmWrite(timer, 1220, true);     //cuenta 1 segundo
  timerAlarmWrite(timer, 73200, true);      //cuenta 1 minuto
  //timerAlarmWrite(timer, 2196000, true);  //cuenta 30 min
  //timerAlarmWrite(timer, 4392000, true);  //cuenta 1 hora
  timerAlarmEnable(timer);

  //setup del timer 1 para parpadeos de 1 Hz
  timer_parp = timerBegin(1, 65536, true);  //timer 0 PS = 65536 f=1220Hz countUp=true
  timerAttachInterrupt(timer_parp, &parp_mantenimiento, true);
  timerAlarmWrite(timer_parp, 1220, true);  //cuenta 1 segundo
  timerAlarmDisable(timer_parp);

  //setup del timer 2 comprobacion del estado del WiFi
  timer_wifi = timerBegin(2, 65536, true);  //timer 0 PS = 65536 f=1220Hz countUp=true
  timerAttachInterrupt(timer_wifi, &check_wifi, true);
  timerAlarmWrite(timer, 80000, true);      //cuenta 1 min y pico
  //timerAlarmWrite(timer_wifi, 1098000, true);  //cuenta 15 min
  timerAlarmDisable(timer_wifi);

  //inicializacion de variable temp_actual
   pt100Value = analogRead(P_pt100);
   //pt100Value = 4096; //MOCK ADC
   //temp_actual = -(89.3256 / 4096.0f) * pt100Value - (0.62616 / 4096.0f);
   temp_preop = -89.3256 * pt100Value;
   temp_actual = temp_preop / 4096.0f;
}

/******************************************************************************************/

void loop() {
  
  if(flag_timer || flag_ACK){ //entrada periodica con timer 0 o asincrona con pulsacion de ACK
    pt100Value = analogRead(P_pt100);
    //pt100Value = 4096; //MOCK ADC
    //temp_actual = -(89.3256 / 4096.0f) * pt100Value - (0.62616 / 4096.0f);
    temp_preop = -89.3256 * pt100Value;
    temp_actual = temp_preop / 4096.0f;
    
    estados_automaticos();
    maquina_estados(estado);
    
    if(flag_timer) flag_timer = 0;
    if(flag_ACK) flag_ACK = 0;
    //Serial.println(pt100Value);
  }

  if(!initW)
    timerAlarmEnable(timer_wifi);
  initW = 1;

   if (millis() > lastTimeBotRan + botRequestDelay)  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while(numNewMessages) {
      Serial.println("got response");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }
  
  delay(100); 
}



/******************************************************************************************/

void maquina_estados(int estado_f) {
  int user=0;
  switch (estado_f) {
    case 0: //arranque inicial
      timerAlarmDisable(timer_parp);
      estado_OK = 0;
      estado_ACK = 0;
      estado_revision = 0;
      flag_alarma = 0;


      digitalWrite(P_LEDOK, LOW);
      digitalWrite(P_LEDALM, LOW);

      digitalWrite(P_BUZZALM, LOW);
      
      if(!estado_arranque){
        for(user=0; user<NUM_USERS; user++){
          Serial.println("Proceso de arranque, esperando hasta 8 horas a que baje de -65 ºC");
           bot.sendMessage(CHAT_ID[user], "Proceso de arranque, esperando hasta 8 horas a que baje de -65 ºC", "");
        }
      }
      
      estado_arranque = 1;
      
      break;

    case 1: //todo OK
      timerAlarmDisable(timer_parp);
      if (estado_OK == 0 && en_mails){
        String result1 = sendEmail(asunto, remitente, "El estado del frigorifico es correcto", "ruben.martinezm@inycom.es", false);
        String result2 = sendEmail(asunto, remitente, "El estado del frigorifico es correcto", "marcos.rodriguez@inycom.es", false);
        String result3 = sendEmail(asunto, remitente, "El estado del frigorifico es correcto", "valledorluis@uniovi.es", false);
        for(user=0; user<NUM_USERS; user++){
          bot.sendMessage(CHAT_ID[user], "El estado del frigorifico es correcto", "");
        }
      }
      estado_arranque = 0;
      estado_OK = 1;
      estado_ACK = 0;
      estado_revision = 0;
      flag_alarma = 0;
      
      //color Verde
      digitalWrite(P_LEDOK, HIGH);
      digitalWrite(P_LEDALM, LOW);

      digitalWrite(P_BUZZALM, LOW);
      break;

    case 2: //alarma SALIDA BUZZER
      timerAlarmDisable(timer_parp);
      if (estado_ACK == 0 && en_mails){
        String result1 = sendEmail(asunto, remitente, "La temperatura del frigorifico es demasiado alta", "ruben.martinezm@inycom.es", false);
        String result2 = sendEmail(asunto, remitente, "La temperatura del frigorifico es demasiado alta", "marcos.rodriguez@inycom.es", false);
        String result3 = sendEmail(asunto, remitente, "La temperatura del frigorifico es demasiado alta", "valledorluis@uniovi.es", false);
        for(user=0; user<NUM_USERS; user++){
          bot.sendMessage(CHAT_ID[user], "La temperatura del frigorifico es demasiado alta", "");
        }
      }
      estado_arranque = 0;
      estado_OK = 0;
      estado_ACK = 1;
      estado_revision = 0;

      digitalWrite(P_BUZZALM, HIGH);
      //color rojo
      digitalWrite(P_LEDOK, LOW);
      digitalWrite(P_LEDALM, HIGH);
      break;

    case 3: //en revision
      timerAlarmEnable(timer_parp);
      if (estado_revision == 0 && en_mails){
        String result1 = sendEmail(asunto, remitente, "El frigorifico se encuentra en revision", "ruben.martinezm@inycom.es", false);
        String result2 = sendEmail(asunto, remitente, "El frigorifico se encuentra en revision", "marcos.rodriguez@inycom.es", false);
        String result3 = sendEmail(asunto, remitente, "El frigorifico se encuentra en revision", "valledorluis@uniovi.es", false);
        for(user=0; user<NUM_USERS; user++){
          bot.sendMessage(CHAT_ID[user], "El frigorifico se encuentra en revision", "");
        }
      }
      estado_arranque = 0;
      estado_OK = 0;
      estado_ACK = 0;
      estado_revision = 1;
      flag_alarma = 0;

      /*naranja parpadeando mediante interrupcion de 1 Hz*/
      
      digitalWrite(P_BUZZALM, LOW);
      
      break;

    case 4: //revisado
      timerAlarmDisable(timer_parp);
      if (estado_revision == 1 && en_mails){
        String result1 = sendEmail(asunto, remitente, "El frigorifico esta revisado", "ruben.martinezm@inycom.es", false);
        String result2 = sendEmail(asunto, remitente, "El frigorifico esta revisado", "marcos.rodriguez@inycom.es", false);
        String result3 = sendEmail(asunto, remitente, "El frigorifico esta revisado", "valledorluis@uniovi.es", false);
        for(user=0; user<NUM_USERS; user++){
          bot.sendMessage(CHAT_ID[user], "El frigorifico esta revisado", "");
        }
      }
      estado_arranque = 0;
      estado_OK = 0;
      estado_ACK = 0;
      estado_revision = 0;
      flag_alarma = 0;

      digitalWrite(P_BUZZALM, LOW);
      
      //color naranja
      digitalWrite(P_LEDOK, HIGH);
      digitalWrite(P_LEDALM, HIGH);

    break;
  } //fin switch
  return;
}

/******************************************************************************************/

void estados_automaticos() {
  if ((estado == 0 && temp_actual <= -65.0f) || (estado == 4 && temp_actual <= -70.0f))
    estado = 1;

  else if ((estado == 0 || estado == 4)&& flag_alarma) //alarma por tiempo al haber entrado N veces en el timer 0 sin haber reiniciado el contador de entrada
    estado = 2;

  else if (estado == 1 && temp_actual >= -60.0f)
    estado = 2;

  else if (estado == 2 && temp_actual <= -70.0f)
    estado = 1;
  return;
}

/******************************************************************************************/

/*
  Al marcar un fragmento de código con el atributo IRAM_ATTR, estamos declarando que el código compilado se colocará en la RAM interna (IRAM) del ESP32.
  De lo contrario, el código se coloca en Flash. Y el flash en el ESP32 es mucho más lento que la RAM interna.
*/
void IRAM_ATTR ISR_ACK() {
  //cambiar estado cuando se pulsa acuse de recibo (ESTADOS MANUALES)
  delay(150); // evita rebotes del pulsador

  flag_ACK = 1;

  if (estado == 4) //falso error
    estado = 1;
    
  else if (estado == 3) //fin mantenimiento
    estado = 4;

  else if (estado == 2) //inicio mantenimiento
    estado = 3;

  else if (estado == 0) //bypass arranque
    estado = 1;
    
  return;
}

/******************************************************************************************/

//Rutina de interrupcion del timer
void IRAM_ATTR onTimer() {

  flag_timer = 1;
  interruptCounter++;
 
  if(estado == 0 || estado == 4){
    
    if(flag_alarma)
      interruptCounter = 0;
  
    if(interruptCounter>=N_TIMERMAX && !flag_alarma)
      flag_alarma = 1;
  }
  else interruptCounter = 0;
}

/******************************************************************************************/

void IRAM_ATTR parp_mantenimiento(){

  if (parp1Hz == LOW) 
      parp1Hz = HIGH; 
  else
      parp1Hz = LOW;
  
  if(estado == 3){ //color naranja
    digitalWrite(P_LEDOK, parp1Hz);
    digitalWrite(P_LEDALM, parp1Hz);
  }
/*
  if(estado == 2)
    digitalWrite(P_BUZZALM, parp1Hz);
    */
}

/******************************************************************************************/

void IRAM_ATTR check_wifi(){
  long int intentos_WIFI = 0;
  int periodo = 1000;
  unsigned long TiempoAhora = 0;
  bool estadoLEDWIFI_f = LOW;
  
  while (WiFi.status() != WL_CONNECTED && intentos_WIFI < N_INTENTOS_WIFI_MAX){
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    intentos_WIFI++;    
    
    if (estadoLEDWIFI_f == LOW) 
      estadoLEDWIFI_f = HIGH; 
    else
      estadoLEDWIFI_f = LOW;
    
    digitalWrite(P_LEDWIFI, estadoLEDWIFI_f); 
    Serial.print(".");

    TiempoAhora = millis();
    while(millis() < TiempoAhora+periodo);  // espere [periodo] milisegundos
  }
  
  if(intentos_WIFI >= N_INTENTOS_WIFI_MAX) 
    digitalWrite(P_LEDWIFI, LOW);
  else {
    digitalWrite(P_LEDWIFI, HIGH);
    Serial.println("WiFi connected.");
  }
}

/******************************************************************************************/

void handleNewMessages(int numNewMessages) {
  //Serial.println("handleNewMessages");
  int user = 0;
  Serial.println(String(numNewMessages));
  for (int i=0; i<numNewMessages; i++) {
    // Chat id of the requester
    String chat_id = String(bot.messages[i].chat_id);

    int j=0;
    while (chat_id != CHAT_ID[j] && j<=NUM_USERS){
      j++;
    }
    if (j>=NUM_USERS){
      j=0;
      bot.sendMessage(chat_id, "Unauthorized user", "");
      continue;
    }

    // Print the received message
    String text = bot.messages[i].text;
    Serial.println(text);
    String from_name = bot.messages[i].from_name;
    
    
    if (text == "/comandos") {
      String welcome = "Bienvenido, " + from_name + ".\n";
      welcome += "Utilice los siguientes comandos para controlar el frigorifico\n\n";
      welcome += "/comandos - Lista comandos \n";
      welcome += "/temperatura - Temperatura actual \n";
      bot.sendMessage(chat_id, welcome, "");
    }

    if (text == "/temperatura") {
      bot.sendMessage(chat_id, String(temp_actual), "");
      //ledState = HIGH;
      //digitalWrite(P_LEDOK, HIGH);
    }

    if (text == "/estado") {
      switch(estado){
        case 0:
          for(user=0; user<NUM_USERS; user++){
            bot.sendMessage(CHAT_ID[user], "Iniciando arranque de la placa de control", "");
          }
        break;

        case 1:
          for(user=0; user<NUM_USERS; user++){
            bot.sendMessage(CHAT_ID[user], "El estado del frigorifico es correcto", "");
          }
        break;

        case 2:
          for(user=0; user<NUM_USERS; user++){
            bot.sendMessage(CHAT_ID[user], "La temperatura del frigorifico es demasiado alta", "");
          }
        break;

        case 3:
          for(user=0; user<NUM_USERS; user++){
            bot.sendMessage(CHAT_ID[user], "El frigorifico se encuentra en revision", "");
          }
        break;

        case 4:
          for(user=0; user<NUM_USERS; user++){
            bot.sendMessage(CHAT_ID[user], "El frigorifico esta revisado", "");
          }
        break;
      }
    }
    
    /*
    if (text == "/led_on") {
      bot.sendMessage(chat_id, "LED state set to ON", "");
      //ledState = HIGH;
      digitalWrite(P_LEDOK, HIGH);
    }
    if (text == "/led_off") {
      bot.sendMessage(chat_id, "LED state set to OFF", "");
      //ledState = LOW;
      digitalWrite(P_LEDOK, LOW);
    }
    if (text == "/state") {
      if (digitalRead(ledPin)){
        bot.sendMessage(chat_id, "LED is ON", "");
      }
      else{
        bot.sendMessage(chat_id, "LED is OFF", "");
      }
    }
    */
    
  }
}
