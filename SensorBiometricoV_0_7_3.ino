/*****************************************************************
   SENSOR BIOMÉTRICO PROFISSIONAL — SB32-PRO-0.7.3
   Cadastro com validação antecipada de nome - Corrigir o (3) Preferences concorrente / usado no callback do Blynk.
*****************************************************************/

#define FIRMWARE_VERSION "SB32-PRO-0.7.3"

const char* URL_VERSION =
"https://raw.githubusercontent.com/Vital-Electronics/SensorBiometrico/main/version.txt";

const char* URL_BIN =
"https://raw.githubusercontent.com/Vital-Electronics/SensorBiometrico/main/SensorBiometrico.bin";

#define BLYNK_PRINT Serial
#define BLYNK_TEMPLATE_ID "TMPL5PDl9gZSo"
#define BLYNK_TEMPLATE_NAME "Terraço"


char auth[] = "LPZ_wQb-AIUfGlljnAoluwZyloNrsmNH";
char ssid[] = "Familia Vital_IoT";
char pass[] = "3289328800";

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <BlynkSimpleEsp32.h>
#include <Adafruit_Fingerprint.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

Preferences prefs;

/* ================================================= HARDWARE ================================================= */

#define RX_SALA 32
#define TX_SALA 33
#define RX_RUA 16
#define TX_RUA 17

#define RELE_SALA 19
#define RELE_RUA 18

#define TEMPO_RELE 3000
#define CAD_TIMEOUT 20000

HardwareSerial salaSerial(1);
HardwareSerial ruaSerial(2);

Adafruit_Fingerprint fingerSala(&salaSerial);
Adafruit_Fingerprint fingerRua(&ruaSerial);

WidgetTerminal terminal(V1);

/* ================================================= ESTRUTURAS ================================================= */

struct BioFSM{
  Adafruit_Fingerprint* sensor;
  bool ativo;
  bool porta;
  unsigned long timer;
  const char* nome;
};

BioFSM sala={&fingerSala,false,false,0,"SALA"};
BioFSM rua ={&fingerRua ,false,false,0,"RUA"};

enum CadState{
  IDLE,
  WAIT_NAME,
  C1,
  REM,
  C2,
  SAVE
};

struct Cadastro{
  CadState estado=IDLE;
  Adafruit_Fingerprint* sensor=nullptr;
  const char* sensorNome="";
  String nome="";
  int id=1;
  unsigned long timer=0;
  bool ativo=false;
}cad;

volatile bool otaRequest=false;
volatile bool otaRunning=false;


enum SeqState {
  SEQ_IDLE,
  SEQ_RUA,
  SEQ_WAIT,
  SEQ_SALA,
  SEQ_SALA_CLOSE
};


SeqState seqState = SEQ_IDLE;
unsigned long seqTimer = 0;



enum DelState {
  DEL_IDLE,
  DEL_WAIT_ID,
  DEL_CONFIRM
};
struct DeleteFSM {
  DelState estado = DEL_IDLE;
  Adafruit_Fingerprint *sensor = nullptr;
  String sensorNome;
  uint16_t id = 0;
};
DeleteFSM del;

// ===================== EXCLUSÃO ASSÍNCRONA (para não travar Blynk) =====================
volatile bool delReq = false;        // pedido pendente
volatile bool delReqDone = false;    // loop concluiu (sucesso/erro)
volatile int  delReqResult = 0;      // FINGERPRINT_OK ou erro

// ===================== LISTAGEM ASSÍNCRONA (para não travar Blynk) =====================
volatile bool listReq = false;
const char*  listReqNome = nullptr;   // "SALA" ou "RUA"




/* ================================================= UI ================================================= */

String qualidade(int r){
  if(r>-67) return "Excelente";
  if(r>-70) return "Bom";
  if(r>-80) return "Ruim";
  return "Péssimo";
}

void header(){

  terminal.clear();
  terminal.printf("🔁 %s\n",FIRMWARE_VERSION);

  if(WiFi.status()==WL_CONNECTED){
    terminal.printf("📡 %s\n",WiFi.SSID().c_str());
    terminal.printf("IP: %s\n",WiFi.localIP().toString().c_str());
    terminal.printf("Sinal: %s\n",qualidade(WiFi.RSSI()).c_str());
  }else terminal.println("📡 WiFi desconectado");

  terminal.println("");
  terminal.flush();
}

/* ================================================= FLASH ================================================= */

void salvarNome(const char* s,int id,String nome){
  prefs.begin("nomes",false);
  prefs.putString((String(s)+"_"+id).c_str(),nome);
  prefs.end();
}

String lerNome(const char* s,int id){
  prefs.begin("nomes",true);
  String n=prefs.getString((String(s)+"_"+id).c_str(),"Desconhecido");
  prefs.end();
  return n;
}

bool nomeExiste(const char* sensor,String nome){

  prefs.begin("nomes",true);

  for(int i=1;i<=200;i++){

    String n=prefs.getString(
      (String(sensor)+"_"+String(i)).c_str(),
      ""
    );

    if(n.equalsIgnoreCase(nome)){
      prefs.end();
      return true;
    }
  }

  prefs.end();
  return false;
}

/* ================================================= CADASTRO FSM ================================================= */

void cadastroFSM(){

  if(!cad.ativo) return;

  if(millis()-cad.timer>CAD_TIMEOUT){

    header();
    terminal.println("❌ Cadastro cancelado");
    terminal.flush();

    cad=Cadastro();
    return;
  }

  switch(cad.estado){

    case WAIT_NAME:
      break;

    case C1:
      if(cad.sensor->getImage()==FINGERPRINT_OK){

        terminal.println("📷 Captura OK");
        terminal.println("👉 Remova o dedo");
        terminal.flush();

        cad.sensor->image2Tz(1);
        cad.estado=REM;
      }
      break;

    case REM:
      if(cad.sensor->getImage()==FINGERPRINT_NOFINGER){

        terminal.println("👉 Coloque novamente");
        terminal.flush();

        cad.estado=C2;
      }
      break;

    case C2:
      if(cad.sensor->getImage()==FINGERPRINT_OK){

        terminal.println("💾 Salvando...");
        terminal.flush();

        cad.sensor->image2Tz(2);
        cad.estado=SAVE;
      }
      break;

    case SAVE:
      if(cad.sensor->createModel()==FINGERPRINT_OK &&
         cad.sensor->storeModel(cad.id)==FINGERPRINT_OK){

        salvarNome(cad.sensorNome,cad.id,cad.nome);

        header();
        terminal.println("✅ Cadastro concluído");
        terminal.flush();

        cad=Cadastro();
      }
      break;

    default: break;
  }
}

/* ================================================= BIOMETRIA ================================================= */

void processaBio(BioFSM &fsm,int rele){

  if(!fsm.ativo || fsm.porta) return;
  if(cad.ativo && cad.sensor==fsm.sensor) return;

  if(fsm.sensor->getImage()==FINGERPRINT_OK){

    if(fsm.sensor->image2Tz()==FINGERPRINT_OK &&
       fsm.sensor->fingerSearch()==FINGERPRINT_OK){

      header();

      terminal.printf("🎯 %s autorizado: %s\n",
        fsm.nome,
        lerNome(fsm.nome,fsm.sensor->fingerID).c_str());

      terminal.flush();

      fsm.sensor->LEDcontrol(
        FINGERPRINT_LED_ON,
        0,
        FINGERPRINT_LED_BLUE
      );

      digitalWrite(rele,HIGH);
      fsm.porta=true;
      fsm.timer=millis();
    }
    else{

      fsm.sensor->LEDcontrol(
        FINGERPRINT_LED_FLASHING,
        100,
        FINGERPRINT_LED_RED
      );

      fsm.timer=millis();
    }
  }
}

void controlePorta(BioFSM &fsm,int rele){

  if(fsm.porta && millis()-fsm.timer>TEMPO_RELE){

    digitalWrite(rele,LOW);
    fsm.porta=false;

    fsm.sensor->LEDcontrol(
      FINGERPRINT_LED_OFF,
      0,
      FINGERPRINT_LED_BLUE
    );
  }

  if(!fsm.porta && millis()-fsm.timer>TEMPO_RELE){

    fsm.sensor->LEDcontrol(
      FINGERPRINT_LED_OFF,
      0,
      FINGERPRINT_LED_RED
    );
  }
}

/* ================================================= LISTAR ================================================= */

void listarSensor(const char* sensorNome){

  header();

  terminal.printf("===== CADASTROS %s =====\n\n",sensorNome);

  prefs.begin("nomes", true);

  for(int i=1;i<=200;i++){

    String nome = prefs.getString(
      (String(sensorNome)+"_"+String(i)).c_str(),
      ""
    );

    if(nome!=""){
      terminal.printf("ID %02d — %s\n",i,nome.c_str());
    }
  }

  prefs.end();

  terminal.println("");
  terminal.flush();
}

/* ================================================= OTA ================================================= */
void checarOTA()
{
  if(otaRunning) return;
  otaRunning = true;

  header();
  terminal.println("🔍 Verificando atualização...");
  terminal.flush();

  if(WiFi.status() != WL_CONNECTED){
    terminal.println("❌ Sem WiFi");
    terminal.flush();
    otaRunning = false;
    return;
  }

  HTTPClient http;
  http.begin(URL_VERSION);

  if(http.GET() != 200){
    terminal.println("❌ Erro HTTP");
    terminal.flush();
    http.end();
    otaRunning = false;
    return;
  }

  String v = http.getString();
  v.trim();
  http.end();

  if(v == FIRMWARE_VERSION){
    terminal.println("✅ Sistema Atualizado.");
    terminal.flush();
    otaRunning = false;
    return;
  }

  terminal.println("⬇ Atualizando...");
  terminal.flush();

  HTTPClient bin;
  bin.begin(URL_BIN);

  if(bin.GET() != 200){
    otaRunning = false;
    return;
  }

  int total = bin.getSize();
  WiFiClient *stream = bin.getStreamPtr();

  Update.begin(total);

  uint8_t buf[1024];
  int escrito = 0;

  int ultimoPrint = -5;   // controle de progresso

  while(bin.connected() && escrito < total)
  {
    int l = stream->readBytes(buf, sizeof(buf));

    if(l){
      Update.write(buf, l);
      escrito += l;

      int progresso = (escrito * 100) / total;

      if(progresso >= ultimoPrint + 5){
        ultimoPrint = (progresso / 5) * 5;

        terminal.printf("%d%%\n", ultimoPrint);
        terminal.flush();
      }
    }

    vTaskDelay(10);
  }

  if(Update.end(true)){
    terminal.println("✅ Download Concluido — Aguarde Reinicio...");
    terminal.flush();
    delay(1000);
    ESP.restart();
  }

  otaRunning = false;
}



/* ================================================= BLYNK ================================================= */

void iniciarCadastro(Adafruit_Fingerprint* sensor,const char* nome){

  cad.ativo=true;
  cad.sensor=sensor;
  cad.sensorNome=nome;
  cad.estado=WAIT_NAME;
  cad.timer=millis();

  header();
  terminal.printf("🔐 Cadastro %s iniciado\n",nome);
  terminal.println("👉 Digite o nome abaixo no terminal");
  terminal.flush();
}


BLYNK_WRITE(V1)
{
  String texto = param.asStr();
  texto.trim();

  // =========================
  // CHAVE SECRETA
  // =========================
  if(texto == "300594")
  {
    header();
    terminal.println("🔓 Abertura Secreta Acionada");
    terminal.flush();

    seqState = SEQ_RUA;
    return;
  }

  // =========================
  // EXCLUSÃO — aguardando ID
  // =========================
  if(del.estado == DEL_WAIT_ID)
  { 
    int id = texto.toInt();

    if(id < 1 || id > 200)
    {
      terminal.println("❌ ID inválido (1..200)");
      terminal.flush();
      return;
    }

    del.id = id;
    del.estado = DEL_CONFIRM;

    terminal.printf("⚠ Confirma eliminar %s ID %d? (s/n)\n",
                    del.sensorNome.c_str(), del.id);
    terminal.flush();
    return;
  }

  // =========================
  // EXCLUSÃO — confirmação
  // =========================
  if(del.estado == DEL_CONFIRM)
  {
    texto.toUpperCase();

    if(texto == "S")
    {
      // NÃO executar deleteModel() aqui (callback do Blynk)!
      // Apenas agenda para o loop principal processar.
      delReq = true;
      delReqDone = false;
      delReqResult = 0;

      terminal.println("🕒 Pedido de exclusão recebido — processando...");
      terminal.flush();

      return; // mantém a FSM em DEL_CONFIRM até o loop concluir
    }
    else
    {
      terminal.println("🚫 Operação cancelada");
      terminal.flush();
      del = DeleteFSM();   // reseta FSM de exclusão
      return;
    }
  }


  // =========================
  // Entrada de nome no cadastro
  // =========================
  if(cad.estado == WAIT_NAME)
  {
    if(texto.length() < 2)
    {
      terminal.println("❌ Nome inválido");
      terminal.flush();
      return;
    }

    if(nomeExiste(cad.sensorNome, texto))
    {
      terminal.println("❌ Nome já existe");
      terminal.flush();

      cad = Cadastro();   // cancela cadastro
      return;
    }

    cad.nome = texto;

    cad.sensor->getTemplateCount();
    cad.id = cad.sensor->templateCount + 1;

    header();
    terminal.println("✔ Nome válido");
    terminal.println("👉 Coloque o dedo no sensor...");
    terminal.flush();

    cad.estado = C1;
    cad.timer  = millis();
    return;
  }

  // =========================
  // Espaço para comandos futuros
  // =========================
}




BLYNK_WRITE(V2){
  if(!cad.ativo) iniciarCadastro(&fingerSala,"SALA");
}


BLYNK_WRITE(V3){
  if(!cad.ativo) iniciarCadastro(&fingerRua,"RUA");
}


BLYNK_WRITE(V6){
  if(param.asInt()){
    listReqNome = "SALA";
    listReq = true;
  }
}

BLYNK_WRITE(V7){
  if(param.asInt()){
    listReqNome = "RUA";
    listReq = true;
  }
}


BLYNK_WRITE(V8)
{
  if(param.asInt() == 0) return;

  del.estado = DEL_WAIT_ID;
  del.sensor = &fingerSala;
  del.sensorNome = "SALA";

  header();
  terminal.println("🗑 Eliminar registro SALA");
  terminal.println("👉 Digite o ID:");
  terminal.flush();
}


BLYNK_WRITE(V9)
{
  if(param.asInt() == 0) return;

  del.estado = DEL_WAIT_ID;
  del.sensor = &fingerRua;
  del.sensorNome = "RUA";

  header();
  terminal.println("🗑 Eliminar registro RUA");
  terminal.println("👉 Digite o ID:");
  terminal.flush();
}



BLYNK_WRITE(V10){
  if(param.asInt()) otaRequest=true;
}









/* ================================================= TRAVA SECRETA ================================================= */
void sequenciaSecreta()
{
  unsigned long agora = millis();

  switch(seqState)
  {
    // ============================
    // Abrir RUA
    // ============================
    case SEQ_RUA:

      digitalWrite(RELE_RUA, HIGH);

      rua.porta = true;
      rua.timer = agora;

      fingerRua.LEDcontrol(FINGERPRINT_LED_ON,0,FINGERPRINT_LED_BLUE);

      terminal.println("🔓 RUA aberta");
      terminal.flush();

      seqTimer = agora;
      seqState = SEQ_WAIT;
      break;

    // ============================
    // Fechar RUA / esperar SALA
    // ============================
    case SEQ_WAIT:

      if(rua.porta && agora - seqTimer >= 3000)
      {
        digitalWrite(RELE_RUA, LOW);
        rua.porta = false;

        fingerRua.LEDcontrol(FINGERPRINT_LED_OFF,0,FINGERPRINT_LED_BLUE);

        terminal.println("🔒 RUA fechada");
        terminal.flush();

        seqTimer = agora;
      }

      if(!rua.porta && agora - seqTimer >= 15000)
      {
        seqState = SEQ_SALA;
      }

      break;

    // ============================
    // Abrir SALA
    // ============================
    case SEQ_SALA:

      digitalWrite(RELE_SALA, HIGH);

      sala.porta = true;
      sala.timer = agora;

      fingerSala.LEDcontrol(FINGERPRINT_LED_ON,0,FINGERPRINT_LED_BLUE);

      terminal.println("🔓 SALA aberta");
      terminal.flush();

      seqTimer = agora;
      seqState = SEQ_SALA_CLOSE;
      break;

    // ============================
    // Fechar SALA
    // ============================
    case SEQ_SALA_CLOSE:

      if(sala.porta && agora - seqTimer >= 3000)
      {
        digitalWrite(RELE_SALA, LOW);
        sala.porta = false;

        fingerSala.LEDcontrol(FINGERPRINT_LED_OFF,0,FINGERPRINT_LED_BLUE);

        terminal.println("🔒 SALA fechada");
        terminal.flush();

        seqState = SEQ_IDLE;
      }

      break;

    // ============================
    // Idle
    // ============================
    case SEQ_IDLE:
      break;
  }
}












/* ================================================= TASK REDE ================================================= */

unsigned long netHealthyTimer = 0; 
const unsigned long NET_TIMEOUT = 60000;
const unsigned long WIFI_RETRY_INTERVAL = 5000; // 5s
unsigned long wifiRetryMs = 0;



void taskRede(void*){

  Blynk.config(auth);
  unsigned long t=0;

  netHealthyTimer=millis();

  for(;;){

   if(WiFi.status() != WL_CONNECTED){

    if(millis() - wifiRetryMs >= WIFI_RETRY_INTERVAL)
      { wifiRetryMs = millis();
        WiFi.disconnect(true);
        WiFi.begin(ssid, pass);
      }

      // não deixa a task girar louca
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }


    if(!Blynk.connected()){
      if(millis()-t>10000){
        t=millis();
        Blynk.connect(2000);
      }
    }else{
      Blynk.run();
    }

    if(otaRequest){
      otaRequest=false;
      checarOTA();
    }

    bool netOK=(WiFi.status()==WL_CONNECTED)&&Blynk.connected();

    if(netOK){
      netHealthyTimer=millis();
    }
    else if(!otaRunning && millis()-netHealthyTimer>NET_TIMEOUT){

      terminal.println("⚠ Rede travada — reboot");
      terminal.flush();

      vTaskDelay(500/portTICK_PERIOD_MS);
      ESP.restart();
    }

    vTaskDelay(20/portTICK_PERIOD_MS);
  }
}

/* ================================================= SETUP ================================================= */

void setup(){

  Serial.begin(115200);

  esp_task_wdt_config_t wdt={
    .timeout_ms=10000,
    .idle_core_mask=(1<<portNUM_PROCESSORS)-1,
    .trigger_panic=true
  };

  esp_task_wdt_init(&wdt);
  esp_task_wdt_add(NULL);

  pinMode(RELE_SALA,OUTPUT);
  pinMode(RELE_RUA,OUTPUT);

  salaSerial.begin(57600,SERIAL_8N1,RX_SALA,TX_SALA);
  ruaSerial.begin(57600,SERIAL_8N1,RX_RUA,TX_RUA);

  sala.ativo=fingerSala.verifyPassword();
  rua.ativo=fingerRua.verifyPassword();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,pass);

  xTaskCreatePinnedToCore(taskRede,"Rede",
  8192,NULL,1,NULL,0);

  delay(1500);
  header();
}

/* ================================================= LOOP ================================================= */

void loop(){

  esp_task_wdt_reset();

  cadastroFSM();

  // ===================== EXECUTA EXCLUSÃO AGENDADA (no loop) =====================
  if(delReq && del.estado == DEL_CONFIRM)
  {
    // executa aqui (loop), não na task do Blynk
    delReqResult = del.sensor->deleteModel(del.id);

    if(delReqResult == FINGERPRINT_OK)
    {
      // remove da memória FLASH (como você já fazia)
      prefs.begin("nomes", false);
      prefs.remove((String(del.sensorNome) + "_" + del.id).c_str());
      prefs.end();
    }

    delReqDone = true;
    delReq = false;
  }



  if(delReqDone && del.estado == DEL_CONFIRM)
  {
    if(delReqResult == FINGERPRINT_OK)
      terminal.println("✅ Registro eliminado com sucesso");
    else
      terminal.println("❌ Falha ao eliminar registro");

    terminal.flush();

    delReqDone = false;
    del = DeleteFSM();   // agora sim reseta FSM de exclusão
  }


  // ===================== EXECUTA LISTAGEM AGENDADA (no loop) =====================
  if(listReq && listReqNome != nullptr)
  {
    listarSensor(listReqNome);
    listReq = false;
    listReqNome = nullptr;
  }



  processaBio(sala,RELE_SALA);
  processaBio(rua ,RELE_RUA);

  controlePorta(sala,RELE_SALA);
  controlePorta(rua ,RELE_RUA);

  sequenciaSecreta();

  delay(5);
}
