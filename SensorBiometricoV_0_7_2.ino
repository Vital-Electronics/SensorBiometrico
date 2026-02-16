/*****************************************************************
   SENSOR BIOMÉTRICO PROFISSIONAL — SB32-PRO-0.7.2
   Melhoria da Versao: Luz purple no cadastro - ascendendo no colocaçao dos dedos.
*****************************************************************/

#define FIRMWARE_VERSION "SB32-PRO-0.7.2"

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

struct BioFSM;  // forward declaration (evita erro de prototype no Arduino)

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <BlynkSimpleEsp32.h>
#include <Adafruit_Fingerprint.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <string.h>

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

/* ================================================= ID MAP (CADASTRO/EXCLUSÃO) =========================================
   Gerenciamento de IDs pelo programa (Preferences) para não depender de templateCount.
   - Bitmap persistente por sensor (namespace "idmap", keys "SALA" e "RUA")
   - Capacidade (maxId) lida do sensor no setup via getParameters() (finger.capacity)
   - Exclusão só libera ID no bitmap se deleteModel(id) no sensor retornar OK
==================================================================================================================== */

static const uint16_t ID_MAX_SUPPORTED = 1000;                   // suporta sensores comuns 200/500/1000
static const uint16_t IDMAP_BYTES = (ID_MAX_SUPPORTED + 7) / 8;  // 125 bytes

uint8_t idmapSala[IDMAP_BYTES];
uint8_t idmapRua [IDMAP_BYTES];

uint16_t maxIdSala = 200; // definido no setup lendo o sensor
uint16_t maxIdRua  = 200; // definido no setup lendo o sensor

static inline uint8_t* idmapPtr(const char* sensorNome){
  return (String(sensorNome)=="SALA") ? idmapSala : idmapRua;
}

static inline uint16_t idmapMax(const char* sensorNome){
  return (String(sensorNome)=="SALA") ? maxIdSala : maxIdRua;
}

static inline bool idmapGet(uint8_t* map, uint16_t id){
  if(id < 1 || id > ID_MAX_SUPPORTED) return true;
  uint16_t idx = (id - 1) >> 3;
  uint8_t bit  = (uint8_t)((id - 1) & 7);
  return (map[idx] & (1 << bit)) != 0;
}

static inline void idmapSet(uint8_t* map, uint16_t id, bool used){
  if(id < 1 || id > ID_MAX_SUPPORTED) return;
  uint16_t idx = (id - 1) >> 3;
  uint8_t bit  = (uint8_t)((id - 1) & 7);
  if(used) map[idx] |=  (1 << bit);
  else     map[idx] &= ~(1 << bit);
}

bool idmapLoad(const char* sensorNome){
  prefs.begin("idmap", true);
  size_t got = prefs.getBytes(sensorNome, idmapPtr(sensorNome), IDMAP_BYTES);
  prefs.end();
  return (got == IDMAP_BYTES);
}

void idmapSave(const char* sensorNome){
  prefs.begin("idmap", false);
  prefs.putBytes(sensorNome, idmapPtr(sensorNome), IDMAP_BYTES);
  prefs.end();
}

// Reconstrói bitmap a partir das chaves existentes em Preferences/nomes (compatível com a base atual)
void idmapRebuildFromNames(const char* sensorNome){
  uint8_t* map = idmapPtr(sensorNome);
  memset(map, 0, IDMAP_BYTES);

  uint16_t mx = idmapMax(sensorNome);
  if(mx < 1) mx = 200;
  if(mx > ID_MAX_SUPPORTED) mx = ID_MAX_SUPPORTED;

  prefs.begin("nomes", true);
  for(uint16_t i=1; i<=mx; i++){
    String key = String(sensorNome) + "_" + String(i);
    String nome = prefs.getString(key.c_str(), "");
    if(nome != ""){
      idmapSet(map, i, true);
    }
  }
  prefs.end();

  idmapSave(sensorNome);
}

int idmapFindFree(const char* sensorNome){
  uint8_t* map = idmapPtr(sensorNome);

  uint16_t mx = idmapMax(sensorNome);
  if(mx < 1) mx = 200;
  if(mx > ID_MAX_SUPPORTED) mx = ID_MAX_SUPPORTED;

  for(uint16_t i=1; i<=mx; i++){
    if(!idmapGet(map, i)) return (int)i;
  }
  return -1;
}


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

  for(int i=1;i<= (int)idmapMax(sensor);i++){

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

  // Timeout / cancelamento: apaga roxo para não ficar preso
  if(millis()-cad.timer > CAD_TIMEOUT){

    if(cad.sensor){
      cad.sensor->LEDcontrol(FINGERPRINT_LED_OFF, 0, FINGERPRINT_LED_PURPLE);
    }

    header();
    terminal.println("❌ Cadastro cancelado");
    terminal.flush();

    cad = Cadastro();
    return;
  }

  switch(cad.estado){

    case WAIT_NAME:
      break;

    case C1:
      if(cad.sensor->getImage() == FINGERPRINT_OK){

        // Captura 1 OK -> roxo fixo
        cad.sensor->LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_PURPLE);

        terminal.println("📷 Captura OK");
        terminal.println("👉 Remova o dedo");
        terminal.flush();

        cad.sensor->image2Tz(1);
        cad.estado = REM;
      }
      break;

    case REM:
      if(cad.sensor->getImage() == FINGERPRINT_NOFINGER){

        // Aguardando segunda captura -> roxo piscando
        cad.sensor->LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_PURPLE);

        terminal.println("👉 Coloque novamente");
        terminal.flush();

        cad.estado = C2;
      }
      break;

    case C2:
      if(cad.sensor->getImage() == FINGERPRINT_OK){

        terminal.println("💾 Salvando...");
        terminal.flush();

        cad.sensor->image2Tz(2);
        cad.estado = SAVE;
      }
      break;

    case SAVE:
      // IMPORTANTE:
      // Não desligar LED aqui enquanto não concluir, senão ele apaga "cedo demais".
      if(cad.sensor->createModel() == FINGERPRINT_OK &&
         cad.sensor->storeModel(cad.id) == FINGERPRINT_OK){

        salvarNome(cad.sensorNome, cad.id, cad.nome);

        idmapSet(idmapPtr(cad.sensorNome), (uint16_t)cad.id, true);
        idmapSave(cad.sensorNome);

        // Conclusão -> gradual off (sem cortar com OFF imediato)
        cad.sensor->LEDcontrol(FINGERPRINT_LED_GRADUAL_OFF, 300, FINGERPRINT_LED_PURPLE);

        header();
        terminal.println("✅ Cadastro concluído");
        terminal.flush();

        cad = Cadastro();
      }
      break;

    default:
      break;
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

  // não mexe no LED do sensor que está em cadastro
  if(cad.ativo && cad.sensor == fsm.sensor) return;

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

  for(int i=1;i<= (int)idmapMax(sensorNome);i++){

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

    if(id <= 0)
    {
      terminal.println("❌ ID inválido");
      terminal.flush();
      return;
    }

    del.id = (uint16_t)id;
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
      if(del.sensor->deleteModel(del.id) == FINGERPRINT_OK)
      {

         // remove da memória FLASH
        prefs.begin("nomes", false);
        prefs.remove((String(del.sensorNome) + "_" + del.id).c_str());
        prefs.end();

        idmapSet(idmapPtr(del.sensorNome.c_str()), del.id, false);
        idmapSave(del.sensorNome.c_str());

        terminal.println("✅ Registro eliminado com sucesso");
      }
      else
      {
        terminal.println("❌ Falha ao eliminar registro");
      }
    }
    else
    {
      terminal.println("🚫 Operação cancelada");
    }

    terminal.flush();
    del = DeleteFSM();   // reseta FSM de exclusão
    return;
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

  //    if(nomeExiste(cad.sensorNome, texto))
  //    {
  //      terminal.println("❌ Nome já existe");
  //      terminal.flush();
  //
  //      cad = Cadastro();   // cancela cadastro
  //      return;
  //    }

    cad.nome = texto;

    int freeId = idmapFindFree(cad.sensorNome);
    if(freeId < 0){
      terminal.println("❌ Memória cheia");
      terminal.flush();
      cad = Cadastro();
      return;
    }
    cad.id = freeId;

    // >>> AQUI: começa o feedback roxo (somente depois do nome)
    cad.sensor->LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_PURPLE);

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
  if(param.asInt()) listarSensor("SALA");
}

BLYNK_WRITE(V7){
  if(param.asInt()) listarSensor("RUA");
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

void taskRede(void*){

  // >>> 0.6.9: taskRede também sob watchdog (evita ficar travada eternamente sem Blynk)
  esp_task_wdt_add(NULL);

  Blynk.config(auth);

  // ============================
  // Robustez em WiFi ruim:
  // - Backoff progressivo WiFi/Blynk
  // - Evita WiFi.disconnect(true) em loop
  // - Reboot vira último recurso (evita reboot-loop)
  // - Netlog em RAM (opcional) e flush ao reconectar
  // - Header garantido no primeiro connect do Blynk
  // - Blynk.run() sempre que WiFi OK (melhora latência/responsividade)
  // - Correção: força Blynk.disconnect() antes de tentar reconectar (evita socket zumbi)
  // ============================

  // Backoff (ms)
  const uint32_t WIFI_BACKOFF[]  = { 2000, 5000, 10000, 30000, 60000 };
  const uint32_t BLYNK_BACKOFF[] = { 2000, 5000, 10000, 30000, 60000 };
  const uint8_t WIFI_BACKOFF_N   = sizeof(WIFI_BACKOFF)  / sizeof(WIFI_BACKOFF[0]);
  const uint8_t BLYNK_BACKOFF_N  = sizeof(BLYNK_BACKOFF) / sizeof(BLYNK_BACKOFF[0]);

  uint32_t lastWifiAttempt  = 0;
  uint32_t lastBlynkAttempt = 0;
  uint8_t  wifiIdx  = 0;
  uint8_t  blynkIdx = 0;

  uint32_t wifiDownSince  = 0;
  uint32_t blynkDownSince = 0;

  // Reboot como último recurso (10 min)
  const uint32_t NET_REBOOT_MIN = 600000UL;

  // Netlog opcional em RAM (somente dentro da task)
  const bool NETLOG_ENABLED = true;
  const uint8_t NETLOG_MAX = 50;

  struct NetLog {
    uint32_t ms;
    const char* msg;
  };

  NetLog netlog[NETLOG_MAX];
  uint8_t netlogHead = 0;
  uint8_t netlogCount = 0;

  auto netlogPush = [&](const char* m){
    if(!NETLOG_ENABLED) return;
    netlog[netlogHead].ms = millis();
    netlog[netlogHead].msg = m;
    netlogHead = (uint8_t)((netlogHead + 1) % NETLOG_MAX);
    if(netlogCount < NETLOG_MAX) netlogCount++;
  };

  auto netlogFlush = [&](){
    if(!NETLOG_ENABLED) return;
    if(!Blynk.connected()) return;
    if(netlogCount == 0) return;

    terminal.println("🧾 Relatório do período offline:");
    terminal.println("--------------------------------");

    uint8_t start = (uint8_t)((netlogHead + NETLOG_MAX - netlogCount) % NETLOG_MAX);
    for(uint8_t i=0;i<netlogCount;i++){
      uint8_t idx = (uint8_t)((start + i) % NETLOG_MAX);
      terminal.printf("[%lus] %s\n", (unsigned long)(netlog[idx].ms/1000UL), netlog[idx].msg);
    }

    terminal.println("--------------------------------");
    terminal.flush();
    netlogCount = 0;
  };

  netHealthyTimer = millis();

  bool lastWifiOK  = (WiFi.status() == WL_CONNECTED);
  bool lastBlynkOK = Blynk.connected();

  bool headerSentOnce = false;

  if(!lastWifiOK)  netlogPush("WiFi desconectado");
  if(!lastBlynkOK) netlogPush("Blynk desconectado");

  for(;;){

    // >>> 0.6.9: alimenta o watchdog desta task
    esp_task_wdt_reset();

    uint32_t now = millis();

    bool wifiOK  = (WiFi.status() == WL_CONNECTED);
    bool blynkOK = Blynk.connected();

    // Transições WiFi
    if(wifiOK != lastWifiOK){
      if(wifiOK){
        wifiDownSince = 0;
        wifiIdx = 0;
        netlogPush("WiFi reconectado");
      }else{
        wifiDownSince = now;
        netlogPush("WiFi desconectou");
      }
      lastWifiOK = wifiOK;
    }

    // Transições Blynk
    if(blynkOK != lastBlynkOK){
      if(blynkOK){
        blynkDownSince = 0;
        blynkIdx = 0;

        header();
        headerSentOnce = true;
        netlogFlush();
      }else{
        blynkDownSince = now;
        netlogPush("Blynk desconectou");
      }
      lastBlynkOK = blynkOK;
    }

    // WiFi reconexão com backoff (sem thrash)
    if(!wifiOK){

      uint32_t backoff = WIFI_BACKOFF[wifiIdx < WIFI_BACKOFF_N ? wifiIdx : (WIFI_BACKOFF_N - 1)];

      if(now - lastWifiAttempt >= backoff){
        lastWifiAttempt = now;

        wl_status_t st = WiFi.status();
        if(st == WL_DISCONNECTED || st == WL_CONNECTION_LOST){
          WiFi.disconnect(false);
          WiFi.begin(ssid, pass);
        }else{
          WiFi.reconnect();
        }

        netlogPush("Tentando reconectar WiFi");

        if(wifiIdx < (WIFI_BACKOFF_N - 1)) wifiIdx++;
      }
    }

    // Blynk: quando WiFi OK, roda SEMPRE para reduzir latência e estabilizar sessão
    if(wifiOK){

      Blynk.run();

      // Se estiver desconectado, tenta reconectar com backoff
      if(!Blynk.connected()){

        uint32_t backoff = BLYNK_BACKOFF[blynkIdx < BLYNK_BACKOFF_N ? blynkIdx : (BLYNK_BACKOFF_N - 1)];

        if(now - lastBlynkAttempt >= backoff){
          lastBlynkAttempt = now;

          // limpa sessão antes de reconectar (evita socket zumbi)
          Blynk.disconnect();
          Blynk.connect(2000);

          netlogPush("Tentando reconectar Blynk");

          if(blynkIdx < (BLYNK_BACKOFF_N - 1)) blynkIdx++;
        }

      }else{
        // Garante header na primeira vez que ficar ON (caso raro de transição perdida)
        if(!headerSentOnce){
          header();
          headerSentOnce = true;
          netlogFlush();
        }
      }
    }

    // OTA sob demanda (mesmo comportamento)
    if(otaRequest){
      otaRequest = false;

      // >>> 0.6.9: OTA pode demorar; remove task do WDT durante checarOTA e recoloca depois
      esp_task_wdt_delete(NULL);
      checarOTA();
      esp_task_wdt_add(NULL);
    }

    // Saúde da rede + fail-safe
    bool netOK = (wifiOK && Blynk.connected());

    if(netOK){
      netHealthyTimer = now;
    }
    else if(!otaRunning){

      bool doReboot = false;

      if(wifiDownSince && (now - wifiDownSince > NET_REBOOT_MIN)){
        doReboot = true;
      }
      if(!wifiDownSince && blynkDownSince && (now - blynkDownSince > NET_REBOOT_MIN)){
        doReboot = true;
      }

      if(doReboot){

        if(Blynk.connected()){
          terminal.println("⚠ Rede travada — reboot");
          terminal.flush();
          vTaskDelay(500/portTICK_PERIOD_MS);
        }else{
          vTaskDelay(500/portTICK_PERIOD_MS);
        }

        ESP.restart();
      }

      (void)NET_TIMEOUT;
      if(now - netHealthyTimer > NET_TIMEOUT){
        // sem ação direta aqui (apenas evita reboot-loop)
      }
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

  // Define capacidade real do sensor para ajustar maxId (evita reescrever firmware ao trocar sensor)
  if(sala.ativo && fingerSala.getParameters() == FINGERPRINT_OK){
    if(fingerSala.capacity > 0){
      maxIdSala = fingerSala.capacity;
      if(maxIdSala > ID_MAX_SUPPORTED) maxIdSala = ID_MAX_SUPPORTED;
    }
  }else{
    maxIdSala = 200;
  }

  if(rua.ativo && fingerRua.getParameters() == FINGERPRINT_OK){
    if(fingerRua.capacity > 0){
      maxIdRua = fingerRua.capacity;
      if(maxIdRua > ID_MAX_SUPPORTED) maxIdRua = ID_MAX_SUPPORTED;
    }
  }else{
    maxIdRua = 200;
  }

  // Carrega bitmap; se não existir, reconstrói a partir de Preferences/nomes (compatível com a base atual)
  bool okSala = idmapLoad("SALA");
  if(!okSala) idmapRebuildFromNames("SALA");

  bool okRua = idmapLoad("RUA");
  if(!okRua) idmapRebuildFromNames("RUA");

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

  processaBio(sala,RELE_SALA);
  processaBio(rua ,RELE_RUA);

  controlePorta(sala,RELE_SALA);
  controlePorta(rua ,RELE_RUA);

  sequenciaSecreta();

  delay(5);
}
