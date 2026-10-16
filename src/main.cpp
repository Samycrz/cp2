#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>

// ---------- PINOS DOS 3 SENSORES (R2) ----------
#define PIN_DHT 15 // Sensor 1: Temperatura (DHT22)
#define PIN_LDR 34 // Sensor 2: Luminosidade (LDR)
#define PIN_POT 35 // Sensor 3: Umidade do solo simulada (Potenciômetro)

// ---------- PINOS DOS 3 ATUADORES (R3) ----------
#define PIN_RELE 26   // Atuador 1: Bomba de irrigação (Relé)
#define PIN_LED 27    // Atuador 2: Luz de crescimento (LED)
#define PIN_BUZZER 14 // Atuador 3: Alarme de superaquecimento (Buzzer)

// ---------- CONFIGURAÇÕES MQTT (R4) ----------
const char *BROKER = "broker.hivemq.com";

// ID Único para evitar bloqueio no broker público HiveMQ
const char *CLIENT_ID = "esp32-551234-559876-yuri-v1";

// 3 Tópicos Publicados (Publish)
const char *PUB_TEMP = "fiap/edge/551234-559876/estufa/sensor/temperatura";
const char *PUB_LUZ = "fiap/edge/551234-559876/estufa/sensor/luminosidade";
const char *PUB_SOLO = "fiap/edge/551234-559876/estufa/evento/irrigacao";

// 3 Tópicos Assinados (Subscribe)
const char *SUB_LUZ = "fiap/edge/551234-559876/estufa/cmd/luz";
const char *SUB_LIMIAR = "fiap/edge/551234-559876/estufa/cmd/limiar_temp";
const char *SUB_MODO = "fiap/edge/551234-559876/estufa/cmd/modo";

WiFiClient net;
PubSubClient mqtt(net);
DHTesp dht;

// Variáveis globais de estado e controle
float temp = 0.0;
int luz = 0;
int solo = 0;
float limiarTemp = 35.0; // Limiar padrão para disparo do alarme na borda
String modo = "AUTO";    // Modo de operação: AUTO ou MANUAL

unsigned long ultimoEnvio = 0;
unsigned long ultimaTentativa = 0;
bool alertaTempImpresso = false;
bool alertaSoloImpresso = false;

// ---------- DECISÃO VIA MQTT (R6) ----------
void callback(char *topic, byte *payload, unsigned int len)
{
  String msg;
  for (unsigned int i = 0; i < len; i++)
    msg += (char)payload[i];
  String t = String(topic);

  Serial.println("[MQTT RECEBIDO] Topico: " + t + " | Payload: " + msg);

  // 1. Comando remoto para ligar/desligar o LED de crescimento
  if (t.endsWith("cmd/luz"))
  {
    if (msg == "ON")
    {
      digitalWrite(PIN_LED, HIGH);
      Serial.println("-> Decisao via MQTT: LED de crescimento LIGADO.");
    }
    else if (msg == "OFF")
    {
      digitalWrite(PIN_LED, LOW);
      Serial.println("-> Decisao via MQTT: LED de crescimento DESLIGADO.");
    }
  }
  // 2. Comando remoto para ajustar o limiar de temperatura da regra de borda
  else if (t.endsWith("cmd/limiar_temp"))
  {
    limiarTemp = msg.toFloat();
    Serial.println("-> Decisao via MQTT: Limiar de temperatura ajustado para " + String(limiarTemp) + " C");
  }
  // 3. Comando remoto para alternar entre modo AUTO e MANUAL
  else if (t.endsWith("cmd/modo"))
  {
    modo = msg;
    Serial.println("-> Decisao via MQTT: Modo alterado para " + modo);
  }
}

// ---------- DECISÃO NA BORDA (R5) ----------
void avaliarRegrasLocais()
{
  // Lê os 3 sensores localmente
  temp = dht.getTemperature();
  luz = map(analogRead(PIN_LDR), 0, 4095, 100, 0);  // Converte para 0 a 100%
  solo = map(analogRead(PIN_POT), 0, 4095, 0, 100); // Converte para 0 a 100%

  // A borda decide sozinha quando está em modo AUTO (mesmo sem internet/broker)
  if (modo == "AUTO")
  {
    // Regra Local 1: Solo seco (< 30%) -> Liga a bomba (Relé) imediatamente
    if (solo < 30)
    {
      digitalWrite(PIN_RELE, HIGH);
      if (!alertaSoloImpresso)
      {
        Serial.println("[BORDA] Decisao local: Solo seco (" + String(solo) + "%). Bomba (Rele) LIGADA!");
        alertaSoloImpresso = true;
      }
    }
    else
    {
      digitalWrite(PIN_RELE, LOW);
      alertaSoloImpresso = false;
    }

    // Regra Local 2: Temperatura acima do limiar -> Aciona alarme (Buzzer) imediatamente
    if (temp > limiarTemp)
    {
      tone(PIN_BUZZER, 1000);
      if (!alertaTempImpresso)
      {
        Serial.println("[BORDA] Decisao local: Temp alta (" + String(temp, 1) + " C > " + String(limiarTemp, 1) + " C). Buzzer LIGADO!");
        alertaTempImpresso = true;
      }
    }
    else
    {
      noTone(PIN_BUZZER);
      alertaTempImpresso = false;
    }
  }
}

// ---------- RECONEXÃO NÃO-BLOQUEANTE ----------
void reconectarSemBloquear()
{
  // Tenta reconectar apenas a cada 5 segundos para não travar a borda
  if (millis() - ultimaTentativa < 5000)
    return;
  ultimaTentativa = millis();

  Serial.print("Conectando ao broker MQTT (HiveMQ)... ");
  if (mqtt.connect(CLIENT_ID))
  {
    Serial.println("Conectado!");
    mqtt.subscribe(SUB_LUZ);
    mqtt.subscribe(SUB_LIMIAR);
    mqtt.subscribe(SUB_MODO);
    Serial.println("Topicos assinados (Subscribe):");
    Serial.println(" - " + String(SUB_LUZ));
    Serial.println(" - " + String(SUB_LIMIAR));
    Serial.println(" - " + String(SUB_MODO));
  }
  else
  {
    Serial.println("Falha na conexao. A borda segue operando localmente.");
  }
}

// ---------- PUBLICAÇÃO PERIÓDICA (PUBLISH) ----------
void publicarLeituras()
{
  if (!mqtt.connected())
    return;

  mqtt.publish(PUB_TEMP, String(temp, 1).c_str());
  mqtt.publish(PUB_LUZ, String(luz).c_str());
  mqtt.publish(PUB_SOLO, (solo < 30) ? "BOMBA_LIGADA" : "SOLO_OK");

  // Log limpo apenas no momento do envio (a cada 2s)
  Serial.print("Enviado MQTT -> Temp: " + String(temp, 1) + "C | Luz: ");
  Serial.print(String(luz) + "% | Solo: ");
  Serial.println(solo < 30 ? "BOMBA_LIGADA" : "SOLO_OK");
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  dht.setup(PIN_DHT, DHTesp::DHT22);

  Serial.print("Conectando ao Wi-Fi Wokwi-GUEST");
  WiFi.begin("Wokwi-GUEST", "");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi conectado! IP: " + WiFi.localIP().toString());

  mqtt.setServer(BROKER, 1883);
  mqtt.setCallback(callback);
  reconectarSemBloquear();
}

void loop()
{
  if (!mqtt.connected())
    reconectarSemBloquear(); // Não trava a borda

  mqtt.loop();
  avaliarRegrasLocais(); // Roda sempre localmente

  if (millis() - ultimoEnvio > 2000)
  {
    publicarLeituras();
    ultimoEnvio = millis();
  }
}